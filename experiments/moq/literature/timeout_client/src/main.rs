//! Original author's MoQ-Lite timeout implementation over actual loopback QUIC.
//! Independent JPEG tiles are an AIoT workload adaptation, not Draco/PCQM.
use anyhow::{Result, anyhow};
use bytes::Bytes;
use moq_lite::{Broadcast, GroupProducer, Origin, Track};
use serde_json::json;
use std::{path::Path, sync::Arc, time::Duration};
use tokio::{
    sync::Mutex,
    time::{Instant, sleep_until},
};

#[tokio::main]
async fn main() -> Result<()> {
    moq_native::Log::new(tracing::Level::WARN).init();
    let args: Vec<String> = std::env::args().collect();
    let server_port: u16 = args[1].parse()?;
    let proxy_port: u16 = args[2].parse()?;
    let timeout_ms: u64 = args[3].parse()?;
    let group_frames: u64 = args[4].parse()?;
    let timeout = if timeout_ms == 0 {
        None
    } else {
        Some(timeout_ms)
    };
    let assets = Path::new(&args[5]);
    let payloads: Vec<Vec<u8>> = (0..5)
        .map(|n| std::fs::read(assets.join(format!("tile-{n}.jpg"))))
        .collect::<std::io::Result<_>>()?;
    let origin = Origin::produce();
    let mut broadcast = Broadcast::produce();
    let mut tracks: Vec<_> = (0..5)
        .map(|i| {
            broadcast.producer.create_track(Track {
                name: format!("tile-{i}"),
                priority: 255 - i,
            })
        })
        .collect();
    origin
        .producer
        .publish_broadcast("tiles", broadcast.consumer);
    let mut cfg = moq_native::ServerConfig::default();
    cfg.bind = Some(format!("127.0.0.1:{server_port}").parse()?);
    cfg.tls.generate = vec!["localhost".to_owned()];
    let mut server = cfg.init()?;
    let serving = tokio::spawn(async move {
        let request = server
            .accept()
            .await
            .ok_or_else(|| anyhow!("server closed"))?;
        let session = request.accept(origin.consumer, None).await?;
        let _ = session.closed().await;
        Ok::<_, anyhow::Error>(())
    });
    let mut client_cfg = moq_native::ClientConfig::default();
    client_cfg.tls.disable_verify = Some(true);
    let client = client_cfg.init()?;
    let mut received_origin = Origin::produce();
    let session = client
        .connect(
            format!("moql://localhost:{proxy_port}").parse()?,
            None,
            received_origin.producer,
        )
        .await?;
    let (_, announced) = received_origin
        .consumer
        .announced()
        .await
        .ok_or_else(|| anyhow!("no announcement"))?;
    let announced = announced.ok_or_else(|| anyhow!("unannouncement"))?;
    let epoch = Instant::now() + Duration::from_millis(500);
    let received = Arc::new(Mutex::new(Vec::new()));
    let mut readers = Vec::new();
    for tile in 0..5u8 {
        let mut track = announced.subscribe_track(
            &Track {
                name: format!("tile-{tile}"),
                priority: 255 - tile,
            },
            timeout,
        );
        let rows = received.clone();
        let expected = Arc::new(payloads[tile as usize].clone());
        readers.push(tokio::spawn(async move{
   let mut groups=tokio::task::JoinSet::new();
   while let Ok(Some(mut group))=track.next_group().await{
    let out=rows.clone();let group_id=group.info.sequence;let expected=expected.clone();
    groups.spawn(async move{
     loop{
      match group.read_frame().await{
       Ok(Some(bytes))=>{
        if bytes.len()<8{break;}
        let frame=u64::from_le_bytes(bytes[..8].try_into().unwrap());
        out.lock().await.push(json!({"event":"description","tile":tile,"frame":frame,"group":group_id,
          "received_s":Instant::now().duration_since(epoch).as_secs_f64(),"bytes":bytes.len(),
          "payload_valid":bytes[8..]==expected[..],"intended_age_s":Instant::now().duration_since(epoch+Duration::from_millis(frame*100)).as_secs_f64()}));
       },
       Ok(None)=>break,
       Err(e)=>{out.lock().await.push(json!({"event":"group_error","tile":tile,"group":group_id,"error":e.to_string()}));break;}
      }
     }
    });
   }
   while groups.join_next().await.is_some(){}
  }));
    }
    let mut groups: Vec<GroupProducer> = Vec::new();
    let mut produced = Vec::new();
    for frame in 0..60u64 {
        sleep_until(epoch + Duration::from_millis(frame * 100)).await;
        produced.push(Instant::now().duration_since(epoch).as_secs_f64());
        if frame % group_frames == 0 {
            for group in groups.drain(..) {
                group.close();
            }
            groups = tracks.iter_mut().map(|t| t.append_group()).collect();
        }
        for (tile, group) in groups.iter_mut().enumerate() {
            let mut payload = frame.to_le_bytes().to_vec();
            payload.extend_from_slice(&payloads[tile]);
            // Chunking exposes the original per-chunk timeout check; it is not QUIC packetization.
            let mut f = group.create_frame(moq_lite::Frame {
                size: payload.len() as u64,
            });
            for chunk in payload.chunks(1024) {
                f.write_chunk(Bytes::copy_from_slice(chunk));
            }
            f.close();
        }
    }
    for group in groups {
        group.close();
    }
    // Keep producers alive while draining so natural latest-two-group eviction,
    // not premature publication withdrawal, determines losses.
    sleep_until(epoch + Duration::from_secs(12)).await;
    let rows = received.lock().await.clone();
    println!(
        "LITERATURE_RESULT {}",
        json!({"offered_frames":60,"descriptions_per_frame":5,"timeout_ms":timeout_ms,
  "group_frames":group_frames,"produced_s":produced,"tile_sizes":payloads.iter().map(|p|p.len()).collect::<Vec<_>>(),
  "scope":"author MoQ-Lite implementation; independent JPEG tile adaptation; not point-cloud reproduction", "events":rows})
    );
    session.close(moq_lite::Error::Cancel);
    for r in readers {
        r.abort();
    }
    serving.abort();
    drop((tracks, broadcast.producer, origin.producer));
    Ok(())
}
