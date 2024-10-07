use std::time::Duration;

async fn work(_: usize) {
    tokio::time::sleep(Duration::from_secs(0)).await;
}

#[tokio::main(flavor = "multi_thread", worker_threads = 1)]
async fn main() {
    {
        // measure how many 0s sleeps it can do in 3s

        println!("rust coro - works in 3s");

        let ts = std::time::Instant::now();

        let mut i = 0;
        _ = tokio::time::timeout(Duration::from_secs(3), async {
            loop {
                work(i).await;
                i += 1;
            }
        })
        .await;

        println!("{} works in 3s", i);
        println!("{}ms", ts.elapsed().as_millis());
    }

    {
        // measure how many ints mpsc can send in 3s

        println!("rust mpsc - works in 3s");

        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();

        let mut send = 0;
        let mut recv = 0;

        let mut acc = 0;

        let ts = std::time::Instant::now();

        let _ = tokio::time::timeout(
            Duration::from_secs(3),
            futures::future::join(
                async {
                    loop {
                        _ = tx.send(0);
                        send += 1;
                        tokio::task::yield_now().await;
                    }
                },
                async {
                    loop {
                        acc += rx.recv().await.unwrap();
                        recv += 1;
                    }
                },
            ),
        )
        .await;

        println!("send throughput {} msg/s", send / 3);
        println!("recv throughput {} msg/s", recv / 3);
        println!("{}ms", ts.elapsed().as_millis());
        println!("acc {}", acc);
    }
}
