Author: Zachary Venzor
CruzID: zvenzor
Course: CSE 156
Assignment: Lab 3 - Reliable UDP File Transfer

This project implements a reliable file transfer application using UDP with a Go-Back-N (GBN) sliding window protocol.

The client reads an input file, splits it into packets with a custom header, and sends them to the server over UDP. The server receives the packets, reorders them if necessary, and writes the reconstructed file to disk based on the path given by the client. Packet drops are simulated using a user-specified percentage, and retransmissions are triggered based on timeouts.

How to Build:
Use the provided Makefile to build both the client and the server:

```bash
make       # builds bin/myclient and bin/myserver
make clean # removes compiled binaries
```

This places `myclient` and `myserver` into the `bin/` directory.

How to Run:
Start the server with a packet drop percentage:

```bash
./myserver <port> <droppc>
```

Run the client:

```bash
./myclient <server_ip> <server_port> <mss> <winsz> <in_file> <out_file_path>
```

If the file is not successfully received and acknowledged, the client exits with an error.

Test Cases Performed:

1. Basic File Round Trip

   * Droppc: 0
   * File: `input.txt`
   * Verified final file matches input using `diff`.

2. Lossy Channel (25% drop)

   * Droppc: 25
   * Verified retransmissions and correct reconstruction.

3. Output Path Includes Directories

   * Output path: `/tmp/nested/output.txt`
   * Verified server creates parent directories as needed.

4. Timeout Handling

   * Server offline during client run.
   * Client exits with timeout after 5 unacked packets.

5. Max Retransmission Test

   * Droppc: 100 (simulate total loss)
   * Verified that client stops after 5 retries and reports error.

Internal Design:

* Go-Back-N protocol with sliding window logic.
* Packet header includes:

  * `int32_t seq_num`
  * `uint8_t type` (INIT, DATA, ACK)
  * `uint8_t is_last`
  * `char cruzid[16]`
* INIT packet from client includes output path and chunk size.
* Server buffers out-of-order packets with a fixed-size buffer.
* Server reassembles file and truncates to final size using `ftruncate()`.
* Retransmission is limited to 5 tries per packet.
* Logging includes window state, ACKs, and retransmissions.
* Server simulates drops on both incoming packets and outgoing ACKs.

Graph Requirement:

"The documentation should include a graph of the sequence numbers and the acknowledge
numbers as seen over time by the client. It must be based on the output log of the client. The
y-axis is the sequence number and the x-axis is the time. The sent sequence numbers would be
plotted as one set and the received ack numbers would be plotted as the other set on the same
graph, based on the log at the client."

|S8               X   X   X   X   X   X   X  A9           
|S7           X   X   X   X   X   X   X   A8 A8          
|S6       X   X   X   X   X   X   X   A7  A7 A7             
|S5   X   X   X   X   X   X   X   A6  A6  A6 A6               
|S4   X   X   X   X   X   X   A5  A5  A5  A5 A5        
|S3   X   X   X   X   X   A4  A4  A4  A4  A4 A4    
|S2   X   X   X   X   A3  A3  A3  A3  A3  A3 A3        
|S1   X   X   A2  A2  A2  A2  A2  A2  A2  A2 A2          
|S0   X   A1  A1  A1  A1  A1  A1  A1  A1  A1 A1               
    ==1 ==2 ==3 ==4 ==5 ==6 ==7 ==8 ==9 ==10==11

Limitations:

* Only one client can interact with the server at a time (effecively)
* Fixed reorder buffer size may drop packets if reordering is excessive.
* File integrity is validated externally using `diff`, not with checksums.

Exit Codes:

| Code | Meaning                            |
| ---- | ---------------------------------- |
| 0    | Success                            |
| 1    | Invalid arguments or MSS too small |
| 2    | File/socket error                  |
| 3    | Memory or path creation error      |
| 4    | Max retransmission reached         |
| 5    | Server down (30s timeout)          |
