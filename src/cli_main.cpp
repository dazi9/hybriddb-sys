#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "hybriddb/btree.hpp"
#include "hybriddb/lsm.hpp"

using namespace hybriddb;

namespace {

void print_usage() {
    std::cout << "Usage:\n"
              << "  hybriddb_cli --engine [btree|lsm] --db <dir> put <key> <value>\n"
              << "  hybriddb_cli --engine [btree|lsm] --db <dir> get <key>\n"
              << "  hybriddb_cli --engine [btree|lsm] --db <dir> del <key>\n"
              << "  hybriddb_cli --engine [btree|lsm] --db <dir> scan [start_key] [end_key]\n"
              << "  hybriddb_cli --engine [btree|lsm] --db <dir> inspect\n"
              << "  hybriddb_cli --engine [btree|lsm] --db <dir> shell\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string engine_type = "lsm";
    std::string db_dir = "./hybriddb_data";
    std::vector<std::string> pos_args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--engine" && i + 1 < argc) {
            engine_type = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            db_dir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            pos_args.push_back(arg);
        }
    }

    if (pos_args.empty()) {
        print_usage();
        return 1;
    }

    std::string cmd = pos_args[0];

    if (engine_type == "btree") {
        BPlusTreeConfig cfg{};
        cfg.db_dir = db_dir;
        BPlusTreeEngine db(cfg);

        if (cmd == "put" && pos_args.size() >= 3) {
            Status st = db.put(pos_args[1], pos_args[2]);
            std::cout << (st.ok() ? "OK" : "ERR: " + st.message) << "\n";
            return st.ok() ? 0 : 1;
        }
        if (cmd == "get" && pos_args.size() >= 2) {
            std::string val;
            Status st = db.get(pos_args[1], &val);
            if (!st.ok()) {
                std::cout << "(nil)\n";
                return 1;
            }
            std::cout << val << "\n";
            return 0;
        }
        if (cmd == "del" && pos_args.size() >= 2) {
            Status st = db.del(pos_args[1]);
            std::cout << (st.ok() ? "1" : "0") << "\n";
            return st.ok() ? 0 : 1;
        }
        if (cmd == "scan") {
            std::string start_k = pos_args.size() >= 2 ? pos_args[1] : "";
            std::string end_k = pos_args.size() >= 3 ? pos_args[2] : "";
            std::vector<std::pair<std::string, std::string>> res;
            db.scan(start_k, end_k, &res);
            for (const auto& [k, v] : res) {
                std::cout << k << "\t" << v << "\n";
            }
            return 0;
        }
        if (cmd == "inspect") {
            auto info = db.inspect_tree();
            std::cout << "B+ Tree Structure Inspection:\n"
                      << "  Tree Height         : " << info.height << "\n"
                      << "  Internal Pages (4KB): " << info.internal_pages << "\n"
                      << "  Leaf Pages (4KB)    : " << info.leaf_pages << "\n"
                      << "  Total Active Slots  : " << info.total_slots << "\n"
                      << "  Avg Leaf Fill Factor: " << std::fixed << std::setprecision(2)
                      << (info.avg_leaf_fill_factor * 100.0) << "%\n"
                      << "  Total Disk Bytes    : " << db.disk_footprint_bytes() << "\n";
            return 0;
        }
        if (cmd == "shell") {
            std::cout << "HybridDB [BPlusTree] REPL. Commands: PUT k v | GET k | DEL k | SCAN [s] [e] | INSPECT | EXIT\n";
            std::string line;
            while (std::cout << "btree> " && std::getline(std::cin, line)) {
                std::istringstream iss(line);
                std::string op, k, v;
                if (!(iss >> op)) continue;
                if (op == "EXIT" || op == "exit" || op == "QUIT" || op == "quit") break;
                if (op == "PUT" || op == "put") {
                    iss >> k >> v;
                    std::cout << (db.put(k, v).ok() ? "OK\n" : "ERR\n");
                } else if (op == "GET" || op == "get") {
                    iss >> k;
                    if (db.get(k, &v).ok()) std::cout << v << "\n";
                    else std::cout << "(nil)\n";
                } else if (op == "DEL" || op == "del") {
                    iss >> k;
                    std::cout << (db.del(k).ok() ? "1\n" : "0\n");
                } else if (op == "INSPECT" || op == "inspect") {
                    auto info = db.inspect_tree();
                    std::cout << "height=" << info.height << " leaves=" << info.leaf_pages
                              << " slots=" << info.total_slots << "\n";
                }
            }
            return 0;
        }
    } else {
        LSMConfig cfg{};
        cfg.db_dir = db_dir;
        LSMTreeEngine db(cfg);

        if (cmd == "put" && pos_args.size() >= 3) {
            Status st = db.put(pos_args[1], pos_args[2]);
            std::cout << (st.ok() ? "OK" : "ERR: " + st.message) << "\n";
            return st.ok() ? 0 : 1;
        }
        if (cmd == "get" && pos_args.size() >= 2) {
            std::string val;
            Status st = db.get(pos_args[1], &val);
            if (!st.ok()) {
                std::cout << "(nil)\n";
                return 1;
            }
            std::cout << val << "\n";
            return 0;
        }
        if (cmd == "del" && pos_args.size() >= 2) {
            Status st = db.del(pos_args[1]);
            std::cout << (st.ok() ? "1" : "0") << "\n";
            return st.ok() ? 0 : 1;
        }
        if (cmd == "scan") {
            std::string start_k = pos_args.size() >= 2 ? pos_args[1] : "";
            std::string end_k = pos_args.size() >= 3 ? pos_args[2] : "";
            std::vector<std::pair<std::string, std::string>> res;
            db.scan(start_k, end_k, &res);
            for (const auto& [k, v] : res) {
                std::cout << k << "\t" << v << "\n";
            }
            return 0;
        }
        if (cmd == "compact") {
            db.compact_all();
            std::cout << "Compaction complete.\n";
            return 0;
        }
        if (cmd == "inspect") {
            auto levels = db.inspect_levels();
            std::cout << "LSM-Tree Level Structure Inspection:\n";
            for (const auto& l : levels) {
                std::cout << "  L" << l.level << ": SSTables=" << l.num_sstables
                          << ", Entries=" << l.total_entries
                          << ", BloomBitsPerKey=" << std::fixed << std::setprecision(2) << l.bits_per_key
                          << ", FilterBytes=" << l.filter_bytes << "\n";
            }
            std::cout << "  Total Disk Bytes: " << db.disk_footprint_bytes() << "\n";
            return 0;
        }
        if (cmd == "shell") {
            std::cout << "HybridDB [LSM-Tree] REPL. Commands: PUT k v | GET k | DEL k | COMPACT | INSPECT | EXIT\n";
            std::string line;
            while (std::cout << "lsm> " && std::getline(std::cin, line)) {
                std::istringstream iss(line);
                std::string op, k, v;
                if (!(iss >> op)) continue;
                if (op == "EXIT" || op == "exit" || op == "QUIT" || op == "quit") break;
                if (op == "PUT" || op == "put") {
                    iss >> k >> v;
                    std::cout << (db.put(k, v).ok() ? "OK\n" : "ERR\n");
                } else if (op == "GET" || op == "get") {
                    iss >> k;
                    if (db.get(k, &v).ok()) std::cout << v << "\n";
                    else std::cout << "(nil)\n";
                } else if (op == "DEL" || op == "del") {
                    iss >> k;
                    std::cout << (db.del(k).ok() ? "1\n" : "0\n");
                } else if (op == "COMPACT" || op == "compact") {
                    db.compact_all();
                    std::cout << "OK\n";
                } else if (op == "INSPECT" || op == "inspect") {
                    for (const auto& l : db.inspect_levels()) {
                        std::cout << "L" << l.level << ": runs=" << l.num_sstables
                                  << " entries=" << l.total_entries
                                  << " bpk=" << l.bits_per_key << "\n";
                    }
                }
            }
            return 0;
        }
    }

    print_usage();
    return 1;
}
