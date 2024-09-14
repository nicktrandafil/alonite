use std::time::Duration;

async fn work(_: usize) {
    tokio::time::sleep(Duration::from_secs(0)).await;
}

#[tokio::main(flavor = "multi_thread", worker_threads = 1)]
async fn main() {
    // measure how many 0s sleeps it can do in 3s

    println!("rust coro - works in 3s");

    let ts = std::time::Instant::now();

    let mut i = 0;
    _ = tokio::time::timeout(Duration::from_secs(3), async {
        while i < 100_000_000 {
            work(i).await;
            i += 1;
        }
    })
    .await;

    println!("{} works in 10s", i);
    println!("{}ms", ts.elapsed().as_millis());
}