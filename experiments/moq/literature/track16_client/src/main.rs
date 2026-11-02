//! Evaluation driver; uses the unmodified MOQtail library and relay switching
//! path. Patches restrict the relay to loopback and normalize client IPv6 literals.
mod cli {
    #[allow(dead_code)]
    pub enum Transport {
        Quic,
        WebTransport,
    }
}
#[path = "../../vendor/moqtail-draft16/apps/client/src/connection.rs"]
mod connection;
use anyhow::{Result, anyhow};
use bytes::Bytes;
use connection::MoqConnection;
use moqtail::model::{
    common::tuple::{Tuple, TupleField},
    control::{
        control_message::ControlMessage, publish::Publish, subscribe::Subscribe, switch::Switch,
        unsubscribe::Unsubscribe,
    },
    data::{object::Object, subgroup_header::SubgroupHeader, subgroup_object::SubgroupObject},
    parameter::message_parameter::MessageParameter,
};
use moqtail::transport::{
    connection::TransportConnection,
    control_stream_handler::ControlStreamHandler,
    data_stream_handler::{HeaderInfo, RecvDataStream, SendDataStream},
};
use serde_json::json;
use std::{collections::BTreeMap, sync::Arc, time::Duration};
use tokio::{
    sync::{Mutex, RwLock},
    time::{Instant, sleep, sleep_until},
};

async fn request(h: &mut ControlStreamHandler, m: ControlMessage) -> Result<ControlMessage> {
    h.send(&m).await.map_err(|e| anyhow!("send {e:?}"))?;
    h.next_message()
        .await
        .map_err(|e| anyhow!("response {e:?}"))
}
fn params() -> Vec<MessageParameter> {
    vec![
        MessageParameter::new_forward(true),
        MessageParameter::new_subscriber_priority(128),
    ]
}
async fn subscribe(h: &mut ControlStreamHandler, name: &str, id: u64) -> Result<u64> {
    let m = request(
        h,
        ControlMessage::Subscribe(Box::new(Subscribe::new_latest_object(
            id,
            Tuple::from_utf8_path("literature"),
            TupleField::from_utf8(name),
            params(),
        ))),
    )
    .await?;
    if let ControlMessage::SubscribeOk(ok) = m {
        Ok(ok.track_alias)
    } else {
        Err(anyhow!("subscribe {m:?}"))
    }
}
async fn receive(
    c: Arc<TransportConnection>,
    epoch: Instant,
    rows: Arc<Mutex<Vec<serde_json::Value>>>,
    expected: Arc<Vec<Bytes>>,
) {
    let fetches = Arc::new(RwLock::new(BTreeMap::new()));
    while let Ok(s) = c.accept_uni().await {
        let f = fetches.clone();
        let out = rows.clone();
        let expected = expected.clone();
        tokio::spawn(async move {
            let handler = RecvDataStream::new(s, f);
            let mut h = &handler;
            loop {
                let (next, obj) = h.next_object().await;
                let Some(o) = obj else { break };
                if o.payload.is_none() {
                    h = next;
                    continue;
                }
                out.lock().await.push(json!({"event":"object","t":epoch.elapsed().as_secs_f64(),"alias":o.track_alias,
      "group":o.location.group,"object":o.location.object,"bytes":o.payload.as_ref().map_or(0,|p|p.len()),"payload_valid":expected.iter().any(|p|Some(p)==o.payload.as_ref())}));
                h = next;
            }
        });
    }
}
async fn produce(
    c: Arc<TransportConnection>,
    epoch: Instant,
    medium: Bytes,
    low: Bytes,
) -> Result<Vec<f64>> {
    let mut handlers: Vec<SendDataStream> = Vec::new();
    let mut produced = Vec::new();
    for frame in 0..100u64 {
        sleep_until(epoch + Duration::from_millis(frame * 100)).await;
        produced.push(epoch.elapsed().as_secs_f64());
        let group = frame / 5;
        let object = frame % 5;
        if object == 0 {
            for h in &mut handlers {
                h.finish().await?;
            }
            handlers.clear();
            for alias in 1..=2 {
                let s = c.open_uni().await?;
                let head =
                    SubgroupHeader::new_with_explicit_id(alias, group, 0, Some(128), true, true);
                handlers.push(
                    SendDataStream::new(
                        Arc::new(Mutex::new(s)),
                        HeaderInfo::Subgroup { header: head },
                    )
                    .await?,
                );
            }
        }
        for (idx, h) in handlers.iter_mut().enumerate() {
            let o = Object::try_from_subgroup(
                SubgroupObject {
                    object_id: object,
                    extension_headers: Some(vec![]),
                    object_status: None,
                    payload: Some(if idx == 0 {
                        medium.clone()
                    } else {
                        low.clone()
                    }),
                },
                idx as u64 + 1,
                group,
                Some(0),
                Some(128),
            )?;
            h.send_object(&o, if object == 0 { None } else { Some(object - 1) })
                .await?;
        }
    }
    for h in &mut handlers {
        h.finish().await?;
    }
    Ok(produced)
}
#[tokio::main]
async fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let mode = &args[1];
    let delay: u64 = args[2].parse()?;
    let port = &args[3];
    assert!(mode == "atomic" || mode == "separate");
    let url = format!("moqt://[::1]:{port}");
    let mut publisher = MoqConnection::establish(&url, true).await?;
    for (id, name, alias) in [(0, "medium", 1), (2, "low", 2)] {
        let m = request(
            &mut publisher.control_stream,
            ControlMessage::Publish(Box::new(Publish::new(
                id,
                Tuple::from_utf8_path("literature"),
                TupleField::from_utf8(name),
                alias,
                vec![MessageParameter::new_forward(true)],
                vec![],
            ))),
        )
        .await?;
        if !matches!(m, ControlMessage::PublishOk(_)) {
            return Err(anyhow!("publish {m:?}"));
        }
    }
    let mut subscriber = MoqConnection::establish(&url, true).await?;
    let old_alias = subscribe(&mut subscriber.control_stream, "medium", 0).await?;
    let epoch = Instant::now();
    let rows = Arc::new(Mutex::new(Vec::new()));
    let medium = Bytes::from(std::fs::read(&args[4])?);
    let low = Bytes::from(std::fs::read(&args[5])?);
    let recv = tokio::spawn(receive(
        subscriber.connection.clone(),
        epoch,
        rows.clone(),
        Arc::new(vec![medium.clone(), low.clone()]),
    ));
    let producer = tokio::spawn(produce(publisher.connection.clone(), epoch, medium, low));
    sleep_until(epoch + Duration::from_secs(3)).await;
    let switch_t = epoch.elapsed().as_secs_f64();
    let new_alias = if mode == "atomic" {
        let kvp = params()
            .into_iter()
            .map(|p| p.try_into())
            .collect::<Result<Vec<_>, _>>()?;
        let m = request(
            &mut subscriber.control_stream,
            ControlMessage::Switch(Box::new(Switch::new(
                2,
                Tuple::from_utf8_path("literature"),
                TupleField::from_utf8("low"),
                0,
                kvp,
            ))),
        )
        .await?;
        if let ControlMessage::SubscribeOk(ok) = m {
            ok.track_alias
        } else {
            return Err(anyhow!("switch {m:?}"));
        }
    } else {
        subscribe(&mut subscriber.control_stream, "low", 2).await?
    };
    let response_t = epoch.elapsed().as_secs_f64();
    if mode == "separate" {
        sleep(Duration::from_millis(delay)).await;
        subscriber
            .control_stream
            .send(&ControlMessage::Unsubscribe(Box::new(Unsubscribe::new(0))))
            .await
            .map_err(|e| anyhow!("unsubscribe {e:?}"))?;
    }
    let produced = producer.await??;
    sleep_until(epoch + Duration::from_secs(16)).await;
    subscriber.connection.close(0, b"finished");
    publisher.connection.close(0, b"finished");
    recv.abort();
    println!(
        "{}",
        json!({"mode":mode,"delay_ms":delay,"switch_request_s":switch_t,"switch_response_s":response_t,
  "produced_s":produced,"old_alias":old_alias,"new_alias":new_alias,"offered_frames":100,"group_frames":5,
  "scope":"author relay actual QUIC; JPEG aligned groups, not original video codec", "objects":*rows.lock().await})
    );
    Ok(())
}
