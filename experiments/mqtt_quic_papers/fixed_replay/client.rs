//! Project fixed-arrival harness over the pinned author's actual MQTT client.
//! This is not the author's benchmark or the complete ESP32 scheduler.
use anyhow::{bail, Result};
use mqtt5::{MqttClient, QoS, SubscribeOptions};
use mqtt5::transport::StreamStrategy;
use serde::{Deserialize, Serialize};
use std::{collections::VecDeque, sync::{Arc, Mutex}, time::{Duration, SystemTime, UNIX_EPOCH}};
use tokio::{sync::Notify, time::{Instant, sleep_until}};

unsafe extern "C" { fn replay_amr_queue_allow(now_us: u64, bytes: u32, queued: i32) -> i32; }

#[derive(Deserialize)]
struct Config {
    url: String, ca: String, strategy: String, qos: u8,
    count: usize, rate: u32, payload: usize, budget: usize,
    deadline_ms: u64, drain_ms: u64, policy: String,
    ready: String, output: String, pause_at_ms: u64, pause_ms: u64,
    active_migration_at_ms: Option<u64>,
}
#[derive(Serialize, Clone)]
struct Generated { seq: usize, due_us: u64, generated_us: u64, decision: String }
#[derive(Serialize)]
struct Received { seq: usize, received_us: u64, latency_us: u64, qos: u8 }
#[derive(Serialize)]
struct Submitted { seq: usize, at_us: u64, api_us: u64, ok: bool, error: Option<String> }
struct Item { seq: usize, payload: Vec<u8> }
#[derive(Default)]
struct State { queue: VecDeque<Item>, bytes: usize, peak: usize, done: bool, generated: Vec<Generated> }

async fn connect(c: &Config, id: &str) -> Result<MqttClient> {
    let client = MqttClient::new(id);
    client.set_tls_config(None, None, Some(std::fs::read(&c.ca)?)).await;
    let strategy = match c.strategy.as_str() {
        "control-only" | "tcp-tls" => StreamStrategy::ControlOnly,
        "per-topic" => StreamStrategy::DataPerTopic,
        "per-publish" => StreamStrategy::DataPerPublish,
        _ => bail!("invalid strategy"),
    };
    client.set_quic_stream_strategy(strategy).await;
    client.connect(&c.url).await?;
    Ok(client)
}

async fn transport_evidence(client: &MqttClient) -> serde_json::Value {
    match client.quic_connection().await {
        Some(conn) => {
            let stats=conn.stats();
            serde_json::json!({"stable_id":conn.stable_id(),"close_reason":conn.close_reason().map(|e|e.to_string()),
                "path_challenge_received":stats.frame_rx.path_challenge,"path_response_sent":stats.frame_tx.path_response,
                "lost_packets":stats.path.lost_packets})
        }
        None => serde_json::Value::Null,
    }
}

#[tokio::main(worker_threads=2)]
async fn main() -> Result<()> {
    let c: Config = serde_json::from_slice(&std::fs::read(std::env::args().nth(1).expect("config"))?)?;
    if c.count==0 || c.count>30000 || c.rate==0 || c.rate>10000 || c.payload<16 ||
       c.payload>1024*1024 || c.budget<c.payload || c.budget>16*1024*1024 ||
       c.qos>1 || !["fifo","deadline","latest","amr_latest_queue_only"].contains(&c.policy.as_str()) {
        bail!("invalid bounded workload");
    }
    let qos=if c.qos==0 {QoS::AtMostOnce} else {QoS::AtLeastOnce};
    let subscriber=connect(&c,"fixed-subscriber").await?;
    let publisher=connect(&c,"fixed-publisher").await?;
    let publisher_monitor=publisher.clone();
    let publisher_start=transport_evidence(&publisher).await;
    let subscriber_start=transport_evidence(&subscriber).await;
    let received=Arc::new(Mutex::new(Vec::<Received>::new()));
    let origin=Arc::new(Mutex::new(None::<Instant>));
    let invalid=Arc::new(Mutex::new(0u64));
    for topic in 0..4 {
        let rx=received.clone(); let origin=origin.clone(); let invalid=invalid.clone();
        let payload_size=c.payload; let count=c.count;
        let (_,granted)=subscriber.subscribe_with_options(format!("fixed/{topic}"),
            SubscribeOptions {qos,..Default::default()}, move |msg| {
                let Some(start)=*origin.lock().unwrap() else {return};
                if msg.payload.len()!=payload_size || msg.qos!=qos {
                    *invalid.lock().unwrap()+=1; return;
                }
                let seq=u64::from_be_bytes(msg.payload[0..8].try_into().unwrap()) as usize;
                let due=u64::from_be_bytes(msg.payload[8..16].try_into().unwrap());
                if seq>=count || seq%4!=topic || msg.payload[16..].iter().any(|v| *v!=(seq%251) as u8) {
                    *invalid.lock().unwrap()+=1; return;
                }
                let now=start.elapsed().as_micros() as u64;
                rx.lock().unwrap().push(Received{seq,received_us:now,latency_us:now.saturating_sub(due),qos:if qos==QoS::AtMostOnce {0}else{1}});
            }).await?;
        if granted!=qos {bail!("wrong SUBACK QoS");}
    }
    // The runner arms its userspace impairment using this common local wall-clock anchor.
    let start=Instant::now()+Duration::from_millis(750);
    let epoch=SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs_f64()+0.750;
    std::fs::write(&c.ready,serde_json::to_vec(&serde_json::json!({"start_epoch_s":epoch}))?)?;
    *origin.lock().unwrap()=Some(start);
    let migration_events=Arc::new(Mutex::new(Vec::<serde_json::Value>::new()));
    let migration_task=c.active_migration_at_ms.map(|at| {
        let events=migration_events.clone(); let p=publisher_monitor.clone();let sub=subscriber.clone();
        tokio::spawn(async move {
            sleep_until(start+Duration::from_millis(at)).await;
            for (role,client) in [("publisher",p),("subscriber",sub)] {
                let result=client.migrate().await;
                events.lock().unwrap().push(serde_json::json!({"role":role,"at_us":start.elapsed().as_micros() as u64,
                    "ok":result.is_ok(),"error":result.err().map(|e|e.to_string())}));
            }
        })
    });
    let state=Arc::new(Mutex::new(State::default()));
    let notify=Arc::new(Notify::new());
    let submitted=Arc::new(Mutex::new(Vec::<Submitted>::new()));
    let s=state.clone(); let n=notify.clone(); let tx=submitted.clone();
    let policy=c.policy.clone(); let deadline=c.deadline_ms;
    let pause_start=start+Duration::from_millis(c.pause_at_ms);
    let pause_end=pause_start+Duration::from_millis(c.pause_ms);
    let worker=tokio::spawn(async move {
        loop {
            let now=Instant::now();
            if now>=pause_start && now<pause_end {sleep_until(pause_end).await;}
            let item={
                let mut s=s.lock().unwrap();
                if let Some(item)=s.queue.pop_front() {Some(item)} else if s.done {break} else {None}
            };
            let Some(item)=item else {n.notified().await; continue};
            let now=start.elapsed().as_micros() as u64;
            let due=u64::from_be_bytes(item.payload[8..16].try_into().unwrap());
            if policy!="fifo" && now.saturating_sub(due)>deadline*1000 {
                let mut s=s.lock().unwrap(); s.bytes-=item.payload.len();
                s.generated[item.seq].decision="expired".into(); continue;
            }
            let size=item.payload.len(); let before=Instant::now();
            // QoS1 method waits for the broker PUBACK; subscriber is measured independently.
            let result=if qos==QoS::AtMostOnce {
                publisher.publish(format!("fixed/{}",item.seq%4),item.payload).await
            } else {
                publisher.publish_qos1(format!("fixed/{}",item.seq%4),item.payload).await
            };
            tx.lock().unwrap().push(Submitted{seq:item.seq,at_us:start.elapsed().as_micros() as u64,
                api_us:before.elapsed().as_micros() as u64,ok:result.is_ok(),error:result.err().map(|e|e.to_string())});
            s.lock().unwrap().bytes-=size;
        }
    });
    for seq in 0..c.count {
        let due_us=seq as u64*1_000_000/u64::from(c.rate);
        sleep_until(start+Duration::from_micros(due_us)).await;
        let generated_us=start.elapsed().as_micros() as u64;
        let mut s=state.lock().unwrap();
        if c.policy=="latest" {
            // Replace only waiting objects of this topic; never cancel an active publish.
            let old=std::mem::take(&mut s.queue);
            for item in old {
                if item.seq%4==seq%4 {
                    s.bytes-=item.payload.len();s.generated[item.seq].decision="superseded".into();
                } else {s.queue.push_back(item);}
            }
        }
        let admitted_by_policy=c.policy!="amr_latest_queue_only" || unsafe {
            replay_amr_queue_allow(generated_us+1,c.payload as u32,i32::from(s.bytes>0))!=0
        };
        let allowed=admitted_by_policy && s.bytes+c.payload<=c.budget;
        s.generated.push(Generated{seq,due_us,generated_us,decision:if allowed {"admitted"} else if !admitted_by_policy {"policy_drop"} else {"budget_drop"}.into()});
        if allowed {
            let mut payload=vec![(seq%251) as u8;c.payload];
            payload[0..8].copy_from_slice(&(seq as u64).to_be_bytes());
            payload[8..16].copy_from_slice(&due_us.to_be_bytes());
            s.queue.push_back(Item{seq,payload});s.bytes+=c.payload;s.peak=s.peak.max(s.bytes);
            notify.notify_one();
        }
    }
    {state.lock().unwrap().done=true;} notify.notify_one();
    let end=start+Duration::from_micros(c.count as u64*1_000_000/u64::from(c.rate))+Duration::from_millis(c.drain_ms);
    sleep_until(end).await;
    let unfinished=!worker.is_finished(); worker.abort(); if let Err(e)=worker.await { if !e.is_cancelled() {bail!("publish worker panicked: {e}");} }
    // Stop callbacks before freezing observations. We deliberately retain cutoff counts.
    let observation_us=start.elapsed().as_micros() as u64;
    *origin.lock().unwrap()=None;
    if let Some(task)=migration_task { task.abort(); if let Err(e)=task.await {if !e.is_cancelled(){bail!("migration task panicked: {e}");}} }
    let publisher_end=transport_evidence(&publisher_monitor).await;
    let subscriber_end=transport_evidence(&subscriber).await;
    let s=state.lock().unwrap();
    std::fs::write(&c.output,serde_json::to_vec_pretty(&serde_json::json!({
        "migration_events":*migration_events.lock().unwrap(),
        "transport_evidence":{"publisher_start":publisher_start,"publisher_end":publisher_end,
            "subscriber_start":subscriber_start,"subscriber_end":subscriber_end},
        "generated":s.generated,"submitted":*submitted.lock().unwrap(),
        "received":*received.lock().unwrap(),"invalid_payloads":*invalid.lock().unwrap(),
        "application_queue_peak_bytes":s.peak,"application_pending_bytes_at_cutoff":s.bytes,
        "worker_pending_at_cutoff":unfinished,"observation_us":observation_us,
        "memory_scope":"application payload queue plus active publish only; transport/broker buffers excluded",
        "clock":"shared process monotonic; latency from scheduled generation to subscriber callback"
    }))?)?;
    Ok(())
}
