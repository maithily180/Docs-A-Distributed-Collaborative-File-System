# Docs++ – Distributed Document Collaboration Platform

**Docs++** is a fully-featured, distributed document collaboration system implemented from scratch in C. The system provides Google Docs-like functionality with support for concurrent editing, fine-grained access control, fault tolerance, and advanced features including hierarchical folder structures, checkpointing, and keyword search.

---

## 📋 Table of Contents

1. [System Overview](#system-overview)
2. [Architecture](#architecture)
3. [Build & Installation](#build--installation)
4. [Quick Start](#quick-start)
5. [Implemented Features](#implemented-features)
6. [Bonus Features](#bonus-features)
7. [Advanced Search (Unique Feature)](#advanced-search-unique-feature)
8. [Protocol & Logging](#protocol--logging)
9. [Fault Tolerance & Recovery](#fault-tolerance--recovery)
10. [Repository Structure](#repository-structure)
11. [Design Decisions](#design-decisions)

---

## 🏗️ System Overview

Docs++ consists of three core components working together to provide a distributed document collaboration platform:

- **Naming Server (NM)** – Central coordinator that maintains file metadata, ownership, access control lists (ACLs), storage locations, and orchestrates operations across storage servers. Handles client authentication, file discovery, and coordinates replication.

- **Storage Server (SS)** – Responsible for persisting file content, managing sentence-level locks for concurrent editing, serving read/write operations, and maintaining undo history. Multiple storage servers can run simultaneously with primary-replica relationships.

- **Client** – Command-line interface providing users with an intuitive way to interact with the system. Supports all file operations, access management, and advanced features.

**Communication Model**: All components communicate via TCP sockets. Clients connect to the NM first; the NM either responds directly or provides the appropriate SS endpoint for direct client-SS sessions (for read/write operations).

---

## 🏛️ Architecture

### Distributed Design
- **Single NM instance** (central coordinator)
- **Multiple SS instances** (primary-replica pairs for fault tolerance)
- **Multiple concurrent clients** (unlimited concurrent users)

### Concurrency Model
- **Sentence-level locking**: Multiple users can edit different sentences simultaneously
- **Per-file, per-sentence locks**: Fine-grained concurrency control
- **Thread-safe operations**: POSIX threads with mutex-protected shared structures

### Data Persistence
- **File content**: Stored in `ss/data/` directory structure
- **Metadata**: Persisted in `nm/metadata.dat` (files, ACLs, users, SS registry)
- **Undo snapshots**: Maintained in `ss/undo/` per file
- **Checkpoints**: Stored in `ss/checkpoints/<filename>/<tag>/`

---

## 🔧 Build & Installation

### Prerequisites
- C compiler (GCC or Clang)
- POSIX-compliant operating system (Linux, macOS, or WSL)
- Make build system

### Build Instructions

```bash
# Navigate to project directory
cd docsplusplus

# Clean previous builds (optional)
make clean

# Build all components
   make all
   ```

This compiles:
- `bin/nm` – Naming Server
- `bin/ss` – Storage Server  
- `bin/client` – Client application

---

## 🚀 Quick Start

### 1. Start the Naming Server
```bash
./bin/nm --host 0.0.0.0 --port 8000 --ss-port 8001
```
Output shows `Name Server listening on 0.0.0.0:8000 ...`. (Optional) Add `--exec-allow` if you explicitly need EXEC to run arbitrary shell commands; otherwise it stays on the safe whitelist (`echo`, `ls`, `pwd`, `dir`, `type`).

### 2. Start Storage Server(s)
```bash
./bin/ss \
  --host 0.0.0.0 \
  --client-port 9000 \
  --admin-port 9100 \
  --nm-ip 127.0.0.1 \
  --nm-port 8001 \
  --ss-id ss1
```
Set `--advertise-ip` if the SS should publish a specific LAN/WAN address; otherwise it uses the interface used to reach the NM. Additional SS instances repeat this command with different `--client-port/--admin-port/--ss-id`.

### 3. Launch Client
```bash
./bin/client --nm-ip 127.0.0.1 --nm-port 8000 --username alice
```
Optional flags: `--client-port` to bind a specific local port and `--bind-ip` to select an interface.

> 💡 `config.yaml` or `config.json` in the repo root can provide defaults (`nm.host`, `nm.port`, `ss.host`, `ss.client_port`, etc.) so you can run the binaries without flags if you prefer.

### 4. Login and Start Using
```
> LOGIN alice
alice> CREATE document.txt
alice> WRITE document.txt 0
1 Hello world.
ETIRW
alice> READ document.txt
```

---

## 🌐 Running Across Multiple Devices (LAN)

Docs++ now ships with full CLI configurability so every component can bind on any interface and dial peers over LAN/WAN links. Here’s a concrete example using two laptops on the same network:

| Role | Device | Sample IP | Command |
|------|--------|-----------|---------|
| Name Server | Laptop A | `172.30.11.205` | `./bin/nm --host 0.0.0.0 --port 8000 --ss-port 8001` |
| Storage Server | Laptop B | `172.29.68.113` | `./bin/ss --host 0.0.0.0 --client-port 9000 --admin-port 9100 --nm-ip 172.30.11.205 --nm-port 8001 --ss-id ss-b --advertise-ip 172.29.68.113` |
| Client | Laptop B (or C) | `172.29.68.113` | `./bin/client --nm-ip 172.30.11.205 --nm-port 8000` |

### Step-by-step LAN workflow
1. **Start the NM on Laptop A**  
   ```bash
   cd /path/to/docsplusplus
   ./bin/nm --host 0.0.0.0 --port 8000 --ss-port 8001
   ```  
   Leave it running; it should print `Name Server listening on 0.0.0.0:8000...`.

2. **Start the SS on Laptop B**  
   ```bash
   cd /path/to/docsplusplus
   ./bin/ss --host 0.0.0.0 \
            --client-port 9000 \
            --admin-port 9100 \
            --nm-ip 172.30.11.205 \
            --nm-port 8001 \
            --ss-id ss-b \
            --advertise-ip 172.29.68.113
   ```  
   Watch Laptop A’s NM console; you should see `Storage Server ss-b registered from 172.29.68.113 (clients 9000 admin 9100)`.

3. **Run the client (Laptop B or C)**  
   ```bash
   cd /path/to/docsplusplus
   ./bin/client --nm-ip 172.30.11.205 --nm-port 8000 --username maithily
   ```  
   Example interaction:
   ```
   CREATE demo.txt
   WRITE demo.txt 0
   0 hello from LAN.
   ETIRW
   READ demo.txt
   STREAM demo.txt
   QUIT
   ```

4. **Confirm routing**  
   - NM console shows `GET_FILE_LOCATION ... SS=172.29.68.113:9000`.
   - SS console logs the READ/WRITE operations.
   - Client output prints the file contents and stream data.

---

### 🌐 Running Across Multiple Devices (LAN / Hotspot)

Below is the simplest correct working setup, based on the real configuration used during testing. This example assumes both laptops are connected to the same mobile hotspot (recommended).

#### 🖥️ Laptop 1 — Naming Server
- Windows IP (from `ipconfig`): `172.20.10.4`
- Commands to run inside Laptop 1 WSL:
  ```bash
  cd /mnt/c/Users/LAPTOP1_USERNAME/Desktop/docsplusplus
  ./bin/nm --host 0.0.0.0 --port 8000 --ss-port 8001
  ```
- Expected output: `Name Server listening on 0.0.0.0:8000 ...`
- Leave this running.

#### 💾 Laptop 2 — Storage Server + Client
- Windows IP: `172.20.10.5`

**Step 1 — Find Laptop 2 WSL IP**
```bash
ip addr show eth0 | grep -oP '(?<=inet\s)\d+(\.\d+){3}'
```
Example output: `172.29.68.113` → call this `WSL_IP`.

**Step 2 — Start Storage Server (Laptop 2 WSL)**
```bash
cd /mnt/c/Users/LAPTOP2_USERNAME/Desktop/docsplusplus
./bin/ss --host 0.0.0.0 \
         --client-port 9000 \
         --admin-port 9100 \
         --nm-ip 172.20.10.4 \
         --nm-port 8001 \
         --ss-id ss-l2 \
         --advertise-ip 172.29.68.113
```
Expected on Laptop 1 NM terminal: `Storage Server ss-l2 registered`

**Step 3 — Run Client (Laptop 2 WSL)**
```bash
cd /mnt/c/Users/LAPTOP2_USERNAME/Desktop/docsplusplus
./bin/client --nm-ip 172.20.10.4 --nm-port 8000
```
Expected:
```
HELLO FROM NM
LOGIN >
```

Try actual operations:
```
LOGIN maithily
CREATE lan.txt
WRITE lan.txt 0
1 Hello from two laptops!
ETIRW
READ lan.txt
STREAM lan.txt
QUIT
```

**🔥 TL;DR (Super Quick Summary)**
- Laptop 1 IP: `172.20.10.4`  
  `./bin/nm --host 0.0.0.0 --port 8000 --ss-port 8001`
- Laptop 2 Windows IP: `172.20.10.5`  
  Laptop 2 WSL IP: `172.29.68.113`
- Storage Server:
  ```bash
  ./bin/ss --host 0.0.0.0 \
           --client-port 9000 \
           --admin-port 9100 \
           --nm-ip 172.20.10.4 \
           --nm-port 8001 \
           --ss-id ss-l2 \
           --advertise-ip 172.29.68.113
  ```
- Client:
  ```bash
  ./bin/client --nm-ip 172.20.10.4 --nm-port 8000
  ```

### NAT & Firewall Notes
- Open/lift inbound firewall rules for NM ports (`8000` client, `8001` SS registration) and SS client/admin ports (`9000/9100`).
- If devices sit behind different NATs, forward the relevant ports on the router and use the router’s public IP in `--advertise-ip` so NM/clients learn how to reach the SS.
- Binding to `0.0.0.0` listens on every interface; swap it for a specific IP if you want to limit exposure.

---

## 🧪 Diagnostics & Network Testing

`net_test.py` (root of this repo) provides two helper modes:

1. **Reachability check**
   ```bash
   python3 net_test.py ping --nm-ip 192.168.1.10 --nm-port 8000 --ss-ip 192.168.1.20 --ss-port 9000
   ```
   Prints per-endpoint latency/errors so you can confirm TCP connectivity/firewall rules before involving the full stack.

2. **Automated round-trip**
   ```bash
   python3 net_test.py roundtrip --nm-ip 127.0.0.1 --nm-client-port 8000 --nm-ss-port 8001 \
       --ss-client-port 9000 --ss-admin-port 9100 --username nettest
   ```
   The script boots local NM + SS binaries, waits for the SS to register, then drives the C client to run `CREATE -> WRITE -> READ`. Logs land in `logs/nettest-*.log`.

---

---

## ✅ Implemented Features

### Core User Functionalities (Required)

#### Authentication & User Management
- **`LOGIN <username>`** – Authenticates user session (case-insensitive usernames)
- **`LIST`** – Displays all registered users in the system

#### File Discovery & Browsing
- **`VIEW`** – Lists all files accessible to the current user
- **`VIEW -a`** – Lists all files in the system (regardless of access permissions)
- **`VIEW -l`** – Lists accessible files with detailed metadata (owner, word count, char count, timestamps)
- **`VIEW -al`** – Lists all system files with detailed metadata
- Flags can be combined (e.g., `-al`)

#### File Lifecycle Operations
- **`CREATE <filename>`** – Creates an empty file owned by the current user
- **`READ <filename>`** – Retrieves and displays complete file content
- **`DELETE <filename>`** – Deletes a file (owner-only operation, blocked if file is locked)
- **`INFO <filename>`** – Displays comprehensive file metadata:
  - Owner information
  - Created and last modified timestamps
  - File size (bytes, words, characters)
  - Access control list (readers and writers)
  - Last accessed information

#### Coordinated Editing
- **`WRITE <filename> <sentence_index>`** – Initiates a write session and locks the specified sentence
  - During the session, provide multiple lines: `<word_index> <content>`
  - Word indices follow 0-based indexing with insertion semantics
  - Content can contain sentence delimiters (`.`, `!`, `?`) which create new sentences
  - **`ETIRW`** – Ends the write session, commits changes, and releases the lock
- **`UNDO <filename>`** – Reverts the last change made to the file (file-specific, not user-specific)

#### Access Control
- **`ADDACCESS -R <filename> <username>`** – Grants read access to a user
- **`ADDACCESS -W <filename> <username>`** – Grants write (and read) access to a user
- **`REMACCESS <filename> <username>`** – Revokes all access for a user
- Owner always has full read/write access

#### Advanced Operations
- **`STREAM <filename>`** – Streams file content word-by-word with 0.1 second delay between words
- **`EXEC <filename>`** – Executes file content as shell commands on the NM and streams output to client

### System Requirements (Required)

#### Data Persistence
- All file content persisted to disk
- Metadata (files, ACLs, users, SS registry) persisted in `nm/metadata.dat`
- Automatic recovery on system restart

#### Access Control Enforcement
- Strict permission checking for all operations
- Owner, reader, and writer lists maintained per file
- Case-insensitive username matching

#### Comprehensive Logging
- All operations logged with timestamps, IP addresses, ports, usernames
- Logs stored in `logs/nm.log` and `logs/ss.log`
- Protocol-level logging for NM-SS communication

---

## 🎁 Bonus Features

### 1. Hierarchical Folder Structure
- **`CREATEFOLDER <foldername>`** – Creates a folder in the file system hierarchy
- **`MOVE <filename> <foldername>`** – Moves a file into a folder (supports nested paths)
- **`VIEWFOLDER <foldername>`** – Displays hierarchical folder structure with proper tree-like indentation
  - Shows folders first (alphabetically), then files (alphabetically)
  - Recursive display of nested folder contents

### 2. Checkpoint System
- **`CHECKPOINT <filename> <checkpoint_tag>`** – Creates a checkpoint snapshot of the file
- **`VIEWCHECKPOINT <filename> <checkpoint_tag>`** – Views the content of a specific checkpoint
- **`REVERT <filename> <checkpoint_tag>`** – Reverts file to a checkpoint version
- **`LISTCHECKPOINTS <filename>`** – Lists all checkpoints for a file with timestamps

### 3. Access Request System
- **`REQUESTACCESS <filename> [-R|-W]`** – Requests read or write access to a file
- **`APPROVE_REQUEST <filename> <requesting_user> [-R|-W]`** – Owner approves a pending access request
- **`VIEW REQUEST`** or **`LISTREQUESTS`** – Lists all pending access requests for files owned by the current user

### 4. Fault Tolerance & Replication
- **Asynchronous replication** – Files automatically replicated to replica SS instances
- **Heartbeat monitoring** – NM tracks SS liveness (30-second timeout)
- **Automatic failure detection** – Failed SS instances marked as inactive
- **SS Recovery** – When an SS reconnects, files are automatically synchronized from replicas
- **Primary-replica architecture** – Each primary SS can have replica SS instances

---

## 🔍 Advanced Search (Unique Feature)

**SEARCH** is our unique bonus feature that enables users to find files containing specific keywords across the entire distributed system.

### Syntax
```
SEARCH <keyword>
```

### Description
The SEARCH command queries all active storage servers in the system to find files containing the specified keyword. The search is performed across all files stored on all storage servers, and results are filtered based on the user's access permissions.

### Features
- **Distributed Search**: Queries all active storage servers simultaneously
- **Access Control**: Only returns files the user has read or write access to
- **Real-time Results**: Aggregates results from all SS instances
- **Duplicate Elimination**: Automatically removes duplicate file entries
- **Permission-Aware**: Respects file ownership and ACLs

### Example Usage
```
alice> SEARCH important
SEARCH RESULTS:
--> project/notes.txt
--> meeting/minutes.txt
--> personal/diary.txt

alice> SEARCH algorithm
SEARCH RESULTS:
--> code/sorting.txt
--> research/papers.txt
```

### Implementation Details
1. Client sends `SEARCH <keyword>` to the NM
2. NM queries all active storage servers via admin port
3. Each SS searches its local files for the keyword
4. SS returns list of matching filenames
5. NM filters results based on user's access permissions
6. NM aggregates and deduplicates results
7. Client receives formatted list of accessible files containing the keyword

### Access Control
- Users only see files they have **read** or **write** access to
- Files owned by the user are always included
- Files in readers/writers lists are included if user matches
- Files without access are filtered out

---

## 📡 Protocol & Logging

### NM-SS Protocol Commands

The system implements comprehensive protocol-level logging for all NM-SS interactions:

- **`REGISTER_SS`** – SS announces itself to NM on startup (includes IP, ports)
- **`HEARTBEAT`** – SS sends periodic heartbeats for liveness monitoring
- **`SS_CREATE`** – NM instructs SS to create a file
- **`SS_DELETE`** – NM instructs SS to delete a file
- **`REPLICATE_FILE`** – NM instructs SS to replicate a file to another SS
- **`GET_FILE_LOCATION`** – NM returns SS location for file operations
- **`REGISTER_CLIENT`** – Client registration logged with IP and port

### Logging Format

All operations are logged with the following format:
```
[YYYY-MM-DD HH:MM:SS] COMPONENT: OPERATION user=<username> details=<operation_details> result=<error_code>
```

Example log entries:
```
[2025-11-19 02:15:46] NM: STARTUP user=SYSTEM details=Naming Server started result=0
[2025-11-19 02:15:51] NM: REGISTER_SS user=ss1 details=REGISTER_SS 127.0.0.1 7101 7201 result=0
[2025-11-19 02:16:02] NM: REGISTER_CLIENT user=SYSTEM details=REGISTER_CLIENT IP=127.0.0.1 Port=46406 result=0
[2025-11-19 02:16:02] NM: LOGIN user=alice details=IP=127.0.0.1 result=0
[2025-11-19 02:16:18] NM: SS_CREATE user=ss1 details=document.txt result=0
[2025-11-19 02:16:18] NM: REPLICATE_FILE user=ss1 details=REPLICATE_FILE document.txt target_ss=ss2 result=0
[2025-11-19 02:16:20] NM: GET_FILE_LOCATION user=alice details=GET_FILE_LOCATION file=document.txt SS=127.0.0.1:7101 result=0
```

---

## 🛡️ Fault Tolerance & Recovery

### Heartbeat Mechanism
- Storage servers send heartbeats every 20 seconds
- Naming server checks SS liveness every 10 seconds
- SS marked as failed if no heartbeat received for 30 seconds

### Failure Detection
- Automatic detection of SS failures
- Failed SS marked as inactive in NM registry
- Operations automatically routed to active SS instances

### Recovery Process
When a storage server reconnects after failure:
1. SS sends REGISTER command to NM
2. NM identifies SS as reconnecting (was previously inactive)
3. NM initiates recovery synchronization:
   - Identifies all files that should be on the recovered SS
   - Finds active replica SS that has up-to-date content
   - Fetches file content from replica using `FETCH` command
   - Synchronizes files to recovered SS using `SYNC` command
4. SS marked as active and ready to serve requests

### Replication Strategy
- **Primary-Replica Model**: Each primary SS can have replica SS instances
- **Asynchronous Replication**: File operations replicated to replicas without blocking
- **Write Propagation**: CREATE, MOVE, and WRITE operations automatically replicated
- **Replica Selection**: Recovery uses replicas or any active SS with file content

---

## 📁 Repository Structure

```
docsplusplus/
├── nm/
│   └── src/
│       └── main.c              # Naming Server implementation
├── ss/
│   └── src/
│       └── main.c              # Storage Server implementation
├── client/
│   └── src/
│       └── main.c              # Client application
├── lib/
│   ├── include/
│   │   ├── net.h               # Network utilities
│   │   ├── error_codes.h       # Universal error codes
│   │   ├── log.h               # Logging system
│   │   ├── util.h               # Utility functions
│   │   ├── hashmap.h           # Hashmap data structure
│   │   └── cache.h             # LRU cache implementation
│   └── src/
│       ├── net.c               # Socket operations
│       ├── error_codes.c        # Error code strings
│       ├── log.c                # Logging implementation
│       ├── util.c               # Utility functions
│       ├── hashmap.c            # Hashmap implementation
│       └── cache.c              # LRU cache implementation
├── bin/                        # Compiled binaries (git-ignored)
├── logs/                       # Log files (git-ignored)
│   ├── nm.log                  # Naming Server logs
│   └── ss.log                  # Storage Server logs
├── nm/
│   └── metadata.dat            # Persistent metadata (git-ignored)
├── ss/
│   ├── data/                   # File storage (git-ignored)
│   ├── undo/                   # Undo snapshots (git-ignored)
│   └── checkpoints/            # Checkpoint storage (git-ignored)
├── Makefile                    # Build configuration
└── README.md                   # This file
```

---

## 🎯 Design Decisions

### Concurrency Control
- **Sentence-level locking**: Enables true concurrent editing (different sentences)
- **Per-file, per-sentence locks**: Fine-grained control without blocking unrelated operations
- **Thread-safe data structures**: Mutex-protected shared state in NM
- **Connection-based locks**: Locks automatically released on disconnect

### Data Structures
- **Hashmap**: O(1) average-case file lookups in NM
- **LRU Cache**: Efficient caching of frequently accessed files
- **Array-based storage**: Simple, efficient file and user management

### Persistence Strategy
- **Atomic writes**: Temporary files + rename for metadata persistence
- **File snapshots**: Undo and checkpoint systems use full file snapshots
- **Metadata serialization**: Binary format for efficient storage and recovery

### Networking
- **Line-oriented protocol**: Simple, debuggable message format
- **TCP sockets**: Reliable, ordered communication
- **Connection pooling**: Efficient resource management

### Error Handling
- **Centralized error codes**: Consistent error reporting across system
- **Graceful degradation**: System continues operating with partial failures
- **Comprehensive logging**: Full audit trail for debugging

---

## 🧪 Testing

### Basic Workflow Test
```bash
# Terminal 1: Start NM
./bin/nm --host 0.0.0.0 --port 8000 --ss-port 8001

# Terminal 2: Start SS
./bin/ss --host 0.0.0.0 --client-port 9000 --admin-port 9100 \
         --nm-ip 127.0.0.1 --nm-port 8001 --ss-id ss-local

# Terminal 3: Start Client
./bin/client --nm-ip 127.0.0.1 --nm-port 8000 --username alice
alice> CREATE test.txt
alice> WRITE test.txt 0
1 Hello world.
ETIRW
alice> READ test.txt
alice> SEARCH Hello
```

### Concurrent Editing Test
```bash
# Start two clients as different users
# Client 1: alice
alice> CREATE shared.txt
alice> WRITE shared.txt 0
1 First sentence.
ETIRW
alice> ADDACCESS -W shared.txt bob

# Client 2: bob
bob> WRITE shared.txt 1
1 Second sentence.
ETIRW
bob> READ shared.txt
```

### Fault Tolerance Test
```bash
# Start NM + SS as above (optionally with a replica)
# Create some files via clients
# Stop SS (Ctrl+C) and observe NM marking it inactive after ~30 seconds
# Restart SS with the same --ss-id; NM triggers file synchronization from replicas/peers
# Verify files are synchronized and clients can READ/WRITE again
```

---

## ✅ Testing Checklist

- [ ] NM binds to `0.0.0.0` (or chosen interface) and accepts connections from remote SS instances and clients.
- [ ] SS registers to the NM using the provided `--nm-ip/--nm-port`, advertising the LAN IP seen by NM.
- [ ] Clients register with NM over LAN (`LOGIN <username>`) and can request file locations remotely.
- [ ] NM responses include the SS IP/`client_port`, and clients perform `READ`/`STREAM` directly against the SS (not via NM).
- [ ] Logs (`logs/nm.log`, `logs/ss.log`) show the remote addresses plus the successful request/response round trip.
- [ ] `CREATE`, `WRITE`, `READ`, and `STREAM` all work across at least two physical devices using the commands documented above.

---

## 📝 Notes

- **Undo depth**: Single-level undo per file (most recent change only)
- **Authentication**: Username-based only (no passwords)
- **File size**: No explicit limits (system handles variable sizes)
- **Sentence delimiters**: Every `.`, `!`, or `?` creates a new sentence (even in words like "e.g.")

---

## 👥 Team

This project was developed as part of the Operating Systems and Networks course project.

---

## 📄 License

This project is developed for educational purposes.

---

**Happy Collaborating! 🚀**
