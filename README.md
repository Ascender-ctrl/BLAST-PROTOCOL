# BLAST File Transfer Protocol over UDP
**Course:** CSc 412 — Computer Networks (Fall 2026)  
**Team:** Isaac Amos & Munif Wahid  

## Build
    make all

## Usage
**Sender:**
    ./sender <receiver_ip> <port> <file_path> <record_size: 256|512|1024> <blast_size_M> <drop_rate_percent>

**Receiver:**
    ./receiver <port> <output_dir>

## Project Milestones
- [x] Shared packet definitions (`protocol.h`) and build system (`Makefile`)
- [x] Sender state machine, `select()` retransmission timers, and `garbler` (`sender-dev`)
- [x] Receiver state machine, closing timer, and 26-test correctness suite (`receiver-dev`)
- [x] Interim Snapshot Report submitted (Nov. 20, 2026)
- [ ] Merge `sender-dev` and `receiver-dev` into `main` & Linux VM validation (Nov. 21–23)
- [ ] Multi-run benchmark sweeps (0%–30% loss) & `matplotlib` plots (Nov. 24–30)
- [ ] Final technical report and code submission (Dec. 4, 2026)
