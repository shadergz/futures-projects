/* Wrote by Gabriel Correia, this code is part of webpro project, read LICENSE and README.md */

/*
 * TODO List
 * [x] - Open a socket and listen on port 80 by default
 * [] - Create a worker for handler connections
 * [] - Registry user into the system
 * [] - Get user information like, client version, name.
 * [] - Send requested user client
*/

/*
 * Tool capabilities
 * [] - Receives multi connections
 * [] - Handles multi connections with worker
 * [] - Attempting to respond various users
 * [] - Threat errors as expected by a server and for user client
*/

/* Open a TCP "AF_INET" connection in port 80 */
use std::net::TcpListener;
use std::sync::{Arc, Mutex};
/* For worker threads */
use std::thread;
use std::thread::{JoinHandle};
use std::time::Duration;

#[allow(dead_code)]
struct WebProServer {
    tcp_listener : TcpListener
}

impl WebProServer {
    pub fn new() -> Self {
        Self {
            /* Opening the server */
            tcp_listener: WebProServer::open_connection().unwrap()
        }
    }

    pub fn shutdown(&mut self) {
        self.drop()
    }

    fn drop(&mut self) {
        WebProServer::close_connection()
    }

    /* Open the server connection and handler all possibles errors */
    fn open_connection() -> Option<TcpListener> {
        let listener_conn = TcpListener::bind("127.0.0.1:80");
        match listener_conn {
            Err(error) => {
                println!("Can't open the server connection, because {}", error);
                None
            },
            Ok(result) => Some(result)
        }
    }

    fn close_connection() {
        println!("The server has closed");
    }
}

const SERVER_NAME: &str = "webpro server has started";
/* We'll make the worker threads sleep for some time, this is essential, we don't want the
 * full CPU time give for us from system scheduler
*/
const SERVER_WORKER_TIME_MS: u64 = 2;

const SERVER_WORKER_COUNT: u64 = 2;

struct WorkerState {
    working: bool
}

/* Worker thread (this is the main component of own server) */
fn worker_thread(server_src: &mut Arc<Mutex<WebProServer>>) -> WorkerState {
    /* Acquires the lock, and so on we will gain access for the web server data */
    let _web_server = server_src.lock().unwrap();
    std::println!("Processing");
    /* It's time to sleep for some time, don't abuse the CPU time */
    thread::sleep(Duration::from_millis(SERVER_WORKER_TIME_MS));
    WorkerState{working: true}
}

/* Initialize communication service */
fn init_conn_service(web_server: &mut Arc<Mutex<WebProServer>>) -> Vec<JoinHandle<Option<bool>>> {
    /* We will spawn two worker here */
    let mut worker_threads = std::vec![];

    for _ in 0..SERVER_WORKER_COUNT {
        let mut server_src = Arc::clone(&web_server);
        let worker_spawn = thread::spawn(move || {
            std::println!("A worker has spawned");
            loop {
                /* Loop endless until the worker has finished his works */
                let worker_info = worker_thread(&mut server_src);
                if worker_info.working == false {
                    break;
                }
            }
            Some(true)
        });
        /* Save worker thread information, so we can control all workers after */
        worker_threads.push(worker_spawn);
    }
    worker_threads
}

/* Wait until all workers being completed */
fn wait_conn_service(workers: Vec<JoinHandle<Option<bool>>>) {
    for worker in workers {
        match worker.join().expect("An error has occurred") {
            None => println!("Worker thread hasn't finished without errors"),
            Some(_id) => println!("Worker thread has finished without errors")
        }
    }
}

fn main() {
    println!("Welcome! {}", SERVER_NAME);
    let mut web_server = Arc::new(Mutex::new(WebProServer::new()));
    let web_workers = init_conn_service(&mut web_server);
    wait_conn_service(web_workers);
    web_server.lock().unwrap().shutdown();
}
