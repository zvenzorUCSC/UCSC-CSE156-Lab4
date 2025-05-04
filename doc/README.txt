Author: Zachary Venzor  
CruzID: zvenzor  
Course: CSE 156  
Assignment: Lab 2 - UDP File Echo

This project implements a UDP-based file echo application consisting of a client and a server.

The client reads an input file, splits it into packets with a custom header, and sends them to the server over UDP. The server echoes the packets back to the client, which reconstructs the output file in the correct order. If the output file does not match the input file after reconstruction, it is deleted automatically.


How to Build:
Use the provided Makefile to build both the client and the server:


This places `myClient` and `myServer` into the `bin/` directory.

How to Run:
Start the server (optionally specifying a port):

./myServer [port]

In another terminal, run the client:

./myClient <server_ip> <server_port> <mss> <input_file_path> <output_file_path>

If the output file does not match the input file after reassembly, it will be deleted.

Test Cases Performed:
1. Large File Transfer
   - Transferred a 500-line text file (~10KB).
   - Verified output file hash matches input file hash.
   - MSS: 1000

2. Variable MSS Sizes
   - Tested with small MSS values (e.g., 100) to force fragmentation.
   - Verified reordering logic and reconstruction integrity.

3. Multiple Clients Simultaneously
   - Ran 3 clients in parallel from separate terminals sending the same file.
   - Verified all output files were correct and independent.

4. Small File Transfer
   - Transferred short text files (under 500 bytes).
   - Verified correct echo and no errors on edge cases.

5. Timeout & Packet Loss Handling
   - Killed server mid-transfer to test timeout.
   - Verified client times out, deletes corrupted file, and exits cleanly.

Internal Design:
- Each packet has a custom header with:
  - Sequence number 
  - CruzID (16 bytes, padded)
- Server echoes  packet (header + payload) back to sender.
- Client reorders packets using a buffer and writes them in correct order.
- Reconstructed file is validated against the input via byte comparison
- If validation fails, the output is deleted

Limitations:
- Reorder buffer is fixed to 5 packets; excessive out-of-order delivery may drop packets.
- No retransmission logic (assumes server echoes reliably).
