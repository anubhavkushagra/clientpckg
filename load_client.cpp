/*
 * load_client.cpp  -  Synchronous benchmark client
 *
 * Usage:  ./load_client <THREADS> <SECONDS> [SERVER_IP] [PAYLOAD_BYTES]
 */
#include "kv.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <iomanip>
#include <vector>

// Helper to read certificate files
std::string ReadFile(const std::string &path) {
  std::ifstream t(path);
  if (!t.is_open())
    return "";
  return std::string((std::istreambuf_iterator<char>(t)),
                     std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cout << "Usage: ./load_client <THREADS> <SECONDS> [SERVER_IP] "
                 "[PAYLOAD_BYTES] [BATCH_SIZE]\n"
              << "  e.g. ./load_client 400 60 192.168.0.109 64 20\n";
    return 0;
  }

  int threads = std::stoi(argv[1]);
  int target_seconds = std::stoi(argv[2]);
  std::string server_ip = (argc >= 4) ? argv[3] : "127.0.0.1";
  int payload_bytes = (argc >= 5) ? std::stoi(argv[4]) : 64;
  int batch_size = (argc >= 6) ? std::stoi(argv[5]) : 10;

  if (batch_size >= 20000) {
      std::cerr << "Batch size cannot exceed keys per shard (20000).\n";
      return 1;
  }

  // SSL/TLS Setup
  std::string ca_cert = ReadFile("ca.crt");
  if (ca_cert.empty())
    ca_cert = ReadFile("../certs/ca.crt"); // Path fallback

  grpc::SslCredentialsOptions ssl_opts;
  ssl_opts.pem_root_certs = ca_cert;
  auto channel_creds = ca_cert.empty() ? grpc::InsecureChannelCredentials()
                                       : grpc::SslCredentials(ssl_opts);

  grpc::ChannelArguments login_args;
  if (!ca_cert.empty()) {
    login_args.SetSslTargetNameOverride("kv-server");
  }

  // Login once
  auto plain_ch = grpc::CreateCustomChannel("ipv4:" + server_ip + ":50063",
                                            channel_creds, login_args);
  auto login_stub = kv::KVService::NewStub(plain_ch);

  std::string jwt;
  {
    grpc::ClientContext ctx;
    kv::LoginRequest req;
    kv::LoginResponse resp;
    req.set_api_key("initial-pass");
    req.set_client_id("bench");
    auto st = login_stub->Login(&ctx, req, &resp);
    if (!st.ok() || resp.jwt_token().empty()) {
      std::cerr << "Login failed: " << st.error_message() << "\n";
      return 1;
    }
    jwt = resp.jwt_token();
    std::cout << "Login OK. Starting benchmark...\n";
  }

  // Stub pool
  const int FIRST_PORT = 50063, LAST_PORT = 50070;
  const int CHANNELS_PER_PORT = 8;
  const int N_PORTS = LAST_PORT - FIRST_PORT + 1;

  std::vector<std::unique_ptr<kv::KVService::Stub>> stubs;
  for (int p = FIRST_PORT; p <= LAST_PORT; ++p) {
    for (int c = 0; c < CHANNELS_PER_PORT; ++c) {
      grpc::ChannelArguments args;
      args.SetMaxReceiveMessageSize(-1);
      args.SetMaxSendMessageSize(-1);
      args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
      if (!ca_cert.empty()) {
        args.SetSslTargetNameOverride("kv-server");
      }
      auto ch = grpc::CreateCustomChannel(
          "ipv4:" + server_ip + ":" + std::to_string(p), channel_creds, args);
      stubs.push_back(kv::KVService::NewStub(ch));
    }
  }

  // Shard-aware key generation
  const long KEYS_PER_SHARD = 20000;
  std::vector<std::vector<std::string>> shard_buckets(N_PORTS);

  std::cout << "Pre-generating " << N_PORTS * KEYS_PER_SHARD << " keys and bucketizing by shard...\n";
  for (int p = 0; p < N_PORTS; ++p) {
      shard_buckets[p].reserve(KEYS_PER_SHARD);
      for (long k = 0; k < KEYS_PER_SHARD; ++k) {
          char buf[64];
          long attempt = 0;
          while (true) {
              int len = std::snprintf(buf, sizeof(buf), "key_p%d_k%ld_a%ld", p, k, attempt++);
              std::string key(buf, len);
              if (std::hash<std::string>{}(key) % N_PORTS == (size_t)p) {
                  shard_buckets[p].push_back(std::move(key));
                  break;
              }
          }
      }
  }

  std::string value(payload_bytes, 'x');
  std::atomic<long> total_ok{0};
  std::atomic<long> total_fail{0};

  auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> pool;
  pool.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t]() {
      long ok = 0, fail = 0, loop_iters = 0;
      unsigned int seed = t;
      
      kv::BatchRequest batch_req;
      kv::BatchResponse batch_resp;
      std::string val(payload_bytes, 'x');

      while (true) {
        if ((++loop_iters & 63) == 0) {
          auto now = std::chrono::steady_clock::now();
          if (now - start >= std::chrono::seconds(target_seconds))
            break;
        }

        int shard_id = rand_r(&seed) % N_PORTS;
        const auto& bucket = shard_buckets[shard_id];

        size_t start_idx = rand_r(&seed) % (bucket.size() - batch_size);

        int stub_idx = (shard_id * CHANNELS_PER_PORT) + (t % CHANNELS_PER_PORT);
        auto* stub = stubs[stub_idx].get();

        batch_req.Clear();
        batch_resp.Clear();
        for (int i = 0; i < batch_size; ++i) {
            auto* req = batch_req.add_requests();
            const std::string& key = bucket[start_idx + i];
            
            int op = (loop_iters + i) % 3;
            if (op == 0) {
                req->set_type(kv::PUT);
                req->set_key(key);
                req->set_value(val);
            } else if (op == 1) {
                req->set_type(kv::GET);
                req->set_key(key);
            } else {
                req->set_type(kv::DELETE);
                req->set_key(key);
            }
        }

        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", jwt);

        auto st = stub->ExecuteBatch(&ctx, batch_req, &batch_resp);
        if (st.ok()) {
            for (int i = 0; i < batch_resp.responses_size(); ++i) {
                if (batch_resp.responses(i).success())
                  ++ok;
                else {
                  ++fail;
                }
            }
        } else {
          fail += batch_size;
          if (fail <= batch_size * 5 && t == 0) {
              std::cerr << "[Thread 0 Error] Batch Execution Failed: " << st.error_message() 
                        << " (Code: " << st.error_code() << ")" << std::endl;
          }
        }
      }
      total_ok.fetch_add(ok, std::memory_order_relaxed);
      total_fail.fetch_add(fail, std::memory_order_relaxed);
    });
  }

  for (auto &th : pool)
    th.join();

  auto end = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(end - start).count();
  double rps = (double)total_ok.load() / elapsed;

  std::cout << "\n--- Benchmark Results ---\n"
            << "  Server IP:    " << server_ip << "\n"
            << "  Threads:      " << threads << "\n"
            << "  Successful:   " << total_ok.load() << "\n"
            << "  Failed:       " << total_fail.load() << "\n"
            << "  Total Time:   " << std::fixed << std::setprecision(2) << elapsed << " s\n"
            << "  Throughput:   " << (long)rps << " req/s\n"
            << "-------------------------\n";
  return 0;
}
