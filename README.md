# Lync
Lync is an open-source, FPGA-targeted trading engine designed for ultra-low latency execution. This project leverages Verilator for cycle-accurate C++ simulation and GTKWave for waveform analysis, providing a sandbox for developing hardware-accelerated trading strategies.

## Tech Stack

* HDL: SystemVerilog / Verilog
* Simulation: Verilator (C++ Wrapper)
* Visualization: GTKWave (VCD/FST)
* Target Hardware: AMD/Xilinx UltraScale+ or Intel Stratix 10

## Key Features


### 1. Protocol parsers

**ITCH 5.0 — market data feed handler**

Nasdaq TotalView-ITCH is a direct data feed product whose specification covers both software and hardware (FPGA) versions of the feed. At the hardware level, a parser for this protocol needs to handle a rich set of message types. The protocol uses a single byte character identifying the message type — for example `S` for System Event, `A` for Add Order — and a 64-bit timestamp representing the event in nanoseconds since midnight, reconstructed from a 6-byte raw timestamp.

The full set of message types the ITCH FSM must decode includes Add Order (with and without MPID attribution), Order Executed, Order Executed with Price, Order Cancel, Order Delete, Order Replace, Trade (non-cross), Trade (cross), Broken Trade, Net Order Imbalance Indicator (NOII), Stock Directory, Stock Trading Action, and System Event messages. All integer fields are big-endian (network byte order) binary encoded numbers, and prices are in fixed-point format with an implied number of decimal places. Instruments are identified by a Stock Locate code — a low-lying integer employed as an array index for rapidly looking up instrument details, present at the same position in all messages to support efficient filtering.

On the transport side, ITCH protocols deliver messages using a higher-level protocol that handles sequencing, re-transmission, and delivery guarantees — either SoupBinTCP or MoldUDP64. In hardware, a state-of-the-art open-source FPGA ITCH parser achieves sub-25 nanosecond latency, processing multiple message types in parallel using speculative decoding.

**OUCH 5.0 — order entry protocol**

OUCH is the low-level native protocol for connecting to Nasdaq, designed to offer maximum possible performance at the cost of flexibility. It allows participants to enter, replace, and cancel orders and receive executions. All numeric fields are binary formatted big-endian numbers: Longs (8 bytes), Integers (4 bytes), Shorts (2 bytes), and Bytes (1 byte).

There are two kinds of messages: inbound (client to host, i.e. the trader sending to Nasdaq) and outbound (host to client). The key inbound messages are Enter Order, Replace Order, and Cancel Order. The key outbound messages are Order Accepted, Order Executed, and Order Cancelled. The `UserRefNum` field in the Enter Order message serves as a strictly-increasing transaction identifier — hardware must maintain and increment this counter per trading session. While ITCH runs over MoldUDP64, OUCH is generally built on top of SoupBinTCP as its sequencing protocol.



### 2. L2 order book

The BRAM-based order book is the architectural centerpiece. It must maintain price-level bid and ask queues for all tracked instruments in real time. With a 1 GHz clock and four parallel decoder modules, parsing latency runs 20–25 ns per message, with order book updates taking 30–40 ns — a total pipeline latency of 100–150 ns processing 8.3M messages per second, utilizing ~35% LUTs and ~25% BRAMs on the target device.

The design maps each instrument's order book into BRAM price-level arrays indexed by price tick. On receipt of an Add Order message, the engine inserts a new entry at the given price level. Order Cancel and Delete messages subtract shares from existing levels. Replace messages atomically remove and re-insert at the new price. Execution messages reduce share counts and remove depleted levels. The book also tracks Best Bid/Offer (BBO) — the top-of-book price and size — with a dedicated register updated on every state change, producing a sub-clock-cycle latency output for strategy logic downstream.

Key design elements include BRAM-based order storage, price-level tables, FIFO buffering, and hardware BBO tracking. Clock domain crossing (CDC) is managed with gray code synchronization. Real-time order book update latency targets sub-microsecond performance.


### 3. Pre-trade risk engine (SEC Rule 15c3-5)

This is the compliance core of the engine. SEC Rule 15c3-5 requires broker-dealers with market access to establish, document, and maintain a system of risk management controls and supervisory procedures that are reasonably designed to systematically limit financial exposure arising from market access.

Specifically, the rule mandates: (i) preventing entry of orders unless all regulatory requirements are satisfied on a pre-order entry basis; (ii) preventing entry of orders for securities a broker-dealer is restricted from trading; (iii) restricting market access technology to authorized persons; and (iv) assuring surveillance personnel receive immediate post-trade execution reports.

In hardware terms, this translates to three wire-speed checks that run in the pipeline before any OUCH Enter Order message is transmitted:

- **Order size check**: compares the proposed quantity against a per-instrument or per-account maximum share threshold stored in BRAM or LUTRAM, rejecting in one clock cycle.
- **Price collar check**: validates that the limit price falls within an acceptable deviation band from a reference price (typically derived from the live order book's BBO). This prevents erroneous orders from being sent at wildly off-market prices.
- **Position limit check**: maintains a running net position per symbol using an accumulator register updated on every execution confirmation received over the OUCH inbound path. New orders are blocked if they would push net exposure beyond configured limits.

The practice of "unfiltered" or "naked" sponsored access is effectively prohibited, since the rule requires that a broker-dealer apply these controls on a pre-trade basis, under the direct and exclusive control of the broker-dealer providing market access. Implementing these in FPGA fabric rather than software satisfies the "direct and exclusive control" requirement while achieving deterministic latency.



### 4. Verilator testbench

Verilator compiles SystemVerilog RTL to a C++ class, enabling simulation at speeds orders of magnitude faster than event-driven simulators like ModelSim, while remaining cycle-accurate. In a Verilator testbench, C++ owns the simulation loop, clock generation, reset sequencing, and FST dumping, while SystemVerilog owns interface declarations, DUT instantiation, assertions, and test logic. Plusargs bridge the two layers at runtime without recompilation.

The PCAP replay component reads captured network traffic files — real Nasdaq ITCH multicast packets — and injects them byte-by-byte into the DUT's AXI-Stream input port, with accurate inter-packet timing reconstructed from PCAP timestamps. This approach provides the advantage of using real-world captured traffic as input, giving the custom IP a confident data input that reflects actual market conditions.

The testbench drives a complete stimulus-response loop: PCAP → ITCH parser → order book → risk engine → OUCH encoder, comparing outputs against a golden C++ reference model. Latency is measured by cycle-counting the delta between a stimulus input timestamp and the resulting OUCH output assertion on the AXI-Stream output bus.


### 5. AXI-Stream pipeline architecture

All internal data movement between pipeline stages uses the AXI4-Stream protocol. Each stage (ITCH parser, order book, risk engine, OUCH encoder) has a `TVALID`/`TREADY` handshake, `TDATA`, `TKEEP`, and `TLAST` signals. Back-pressure propagates upstream so no data is lost under burst conditions. The pipeline runs at a single clock domain on the critical path (targeting 300–500 MHz on UltraScale+ fabric), with clock domain crossing handled explicitly via gray-code synchronization FIFOs where necessary for PCIe or management interfaces.

Decoding protocols like ITCH or OUCH at the hardware level allows faster processing of incoming market updates, and updating bid/ask levels on the FPGA significantly reduces the time needed to react to price changes.



### 6. Target hardware

**AMD/Xilinx UltraScale+**: The VU9P and VU13P variants (used by Xilinx's own Accelerated Algorithmic Trading reference design) offer up to 2.5M logic cells, thousands of DSP slices, tens of megabytes of BRAM, and 100G-capable GTY transceivers directly accessible from the fabric — no external MAC required. Connecting critical interfaces like Ethernet/QSFP and PCI Express directly to the FPGA fabric enables market data feeds captured from the network interface to be immediately processed inside FPGA fabric, accommodating hundreds of parallel processors specialized for each task.

**Intel Stratix 10**: Comparable to UltraScale+ in density, the Stratix 10 GX/SX line supports 58 Gbps transceivers and HBM2 integration on select packages, useful if the order book scales to require ultra-high-bandwidth memory beyond what BRAM can provide.


### 7. GTKWave waveform analysis

GTKWave reads VCD (Value Change Dump) or FST (Fast Signal Trace) files emitted by the Verilator simulation. Engineers use it to visually inspect the state of every internal register and bus at each clock cycle — verifying parser FSM transitions, observing order book state updates, confirming risk check block signals, and precisely measuring tick-to-trade latency by placing cursors on the input stimulus edge and the output OUCH valid assertion. FST is preferred over VCD for large captures because it offers significantly better compression and faster load times.



## End-to-end latency budget

A pipelined design breaks work across simultaneous stages: cycle 0 runs the parser, cycle 1 runs the order book update while a new packet enters the parser, cycle 2 runs the signal logic while the book updates, and cycle 3 runs the risk check while the signal evaluates — all simultaneously. With 4 pipeline stages at a 500 MHz clock (2 ns/cycle), the theoretical minimum tick-to-trade latency is around 8–20 ns for the logic, with network serialization adding tens of nanoseconds depending on packet size and physical link speed. By contrast, a CPU-equivalent implementation demonstrates 38 ± 22 µs with a long tail, illustrating the variability inherent in host OS processing.