# In-Memory Encrypted Virtual RAM-Disk Block Driver

A Linux kernel-space virtual block device that provides temporary RAM-backed storage with transparent AES-XTS encryption.

The project implements a custom Linux block-device driver that exposes a 16 MiB virtual disk as `/dev/secure_ram`. The storage is backed by system RAM rather than persistent media, and block data is encrypted before being stored in the RAM-backed buffer.

---

## Project Overview

Traditional storage devices retain their contents on persistent media such as SSDs or HDDs. A RAM disk takes the opposite approach by using volatile system memory as storage.

This project extends that concept by creating an encrypted RAM-backed block device inside the Linux kernel.

The system allows Linux to treat a region of RAM as a normal block storage device:

```text
/dev/secure_ram

A filesystem such as ext4 can be created on the device and mounted normally. Applications can then perform ordinary file operations without directly handling encryption.
The storage path is:
Application
    ↓
Linux VFS
    ↓
ext4
    ↓
Linux Block Layer
    ↓
BIO
    ↓
secure_ram.ko
    ↓
AES-XTS
    ↓
RAM-backed storage

For reads, the process occurs in the opposite direction, with ciphertext decrypted before the data is returned to the filesystem.
Objectives
The main objectives of the project are:
- Implement a custom Linux kernel module.
- Create a RAM-backed virtual block device.
- Expose the device through /dev/secure_ram.
- Support block-level read and write operations.
- Allow a standard Linux filesystem such as ext4 to operate on the device.
- Integrate the Linux Kernel Crypto API.
- Use AES-256-XTS for transparent block encryption and decryption.
- Keep encrypted data in the RAM-backed storage area.
- Validate correctness using file-integrity and round-trip tests.
- Measure the performance of the encrypted RAM disk against the VM's conventional filesystem.
Key Features
RAM-Backed Storage
The virtual device provides 16 MiB of storage backed by kernel-managed RAM.
The storage is volatile and is not intended for persistent data.
Linux Block Device
The driver registers a block device that appears as:
/dev/secure_ram

Linux can therefore interact with it through normal block-device mechanisms.
ext4 Filesystem Support
The device can be formatted using:
sudo mkfs.ext4 /dev/secure_ram

and mounted like a normal filesystem:
sudo mount /dev/secure_ram /mnt

After mounting, normal file operations can be performed.
Transparent Encryption
Applications do not explicitly call encryption or decryption routines.
For a write:
Plaintext
    ↓
Filesystem
    ↓
Block I/O
    ↓
Driver
    ↓
AES-XTS Encryption
    ↓
Ciphertext
    ↓
RAM

For a read:
RAM
    ↓
Ciphertext
    ↓
AES-XTS Decryption
    ↓
Driver
    ↓
Filesystem
    ↓
Plaintext

AES-256-XTS
The project uses the Linux Kernel Crypto API with:
xts(aes)

and a 64-byte key suitable for AES-256-XTS.
The encryption key is generated in kernel space when the module is initialized and cleared when the module is unloaded.
Volatile Storage
The RAM-backed contents disappear when the storage allocation is destroyed, such as during module teardown or system shutdown.
The project is therefore intended as temporary storage.
Debugging and Verification
A read-only debugfs interface is used during development to inspect the backing storage:
/sys/kernel/debug/secure_ram/raw_storage

This makes it possible to verify that the RAM-backed storage contains ciphertext rather than the original plaintext.
Architecture
The major components are:
USER SPACE
─────────────────────────────────────

Applications / CLI tools
        │
        ▼
Linux VFS
        │
        ▼
ext4 filesystem


KERNEL SPACE
─────────────────────────────────────

Linux Block Layer
        │
        ▼
BIO requests
        │
        ▼
/dev/secure_ram
        │
        ▼
secure_ram.ko
        │
        ├── submit_bio()
        │
        ├── secure_ram_wq
        │
        ├── process_bio()
        │
        └── sector processing
                    │
                    ▼
             AES-XTS Crypto API
                    │
                    ▼
              RAM-backed storage


VOLATILE MEMORY
─────────────────────────────────────

16 MiB RAM
    ↓
Encrypted ciphertext

Read and Write Processing
Write Path
When an application writes a file:
Application
    ↓
VFS
    ↓
ext4
    ↓
Linux Block Layer
    ↓
BIO
    ↓
secure_ram_submit_bio()
    ↓
secure_ram_wq
    ↓
process_bio()
    ↓
Sector processing
    ↓
AES-XTS encryption
    ↓
Ciphertext stored in RAM

Read Path
When an application reads a file:
RAM
    ↓
Ciphertext
    ↓
AES-XTS decryption
    ↓
Sector processing
    ↓
process_bio()
    ↓
secure_ram_wq
    ↓
BIO
    ↓
Linux Block Layer
    ↓
ext4
    ↓
VFS
    ↓
Application

Technologies Used
Programming
- C
- Linux Kernel APIs
Kernel / Storage
- Linux Kernel Module
- Linux Block Layer
- BIO-based block I/O
- gendisk
- block_device_operations
- Kernel workqueue
- Kernel memory allocation
- debugfs
Filesystem
- ext4
Cryptography
- Linux Kernel Crypto API
- AES-256-XTS
Build System
- Linux kbuild
- Make
- GCC
Development Environment
- Ubuntu Linux
- Virtual Machine
- SSH
- Git / GitHub
Requirements
A Linux development environment with:
- GCC
- Make
- Matching Linux kernel headers
- kmod
- Git
Install the basic development environment on Ubuntu:
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) kmod git

Build
Clone the repository:
git clone git@github.com:943ak/Encrypted-Virtual-RAM-Disk-Block-Driver.git
cd Encrypted-Virtual-RAM-Disk-Block-Driver

Build the kernel module:
make

The resulting kernel module is:
secure_ram.ko

Generated kernel build artifacts are excluded through .gitignore.
Load the Driver
Load the module:
sudo insmod ./secure_ram.ko

Verify that the module is loaded:
lsmod | grep secure_ram

Check kernel messages:
sudo dmesg | tail -n 20

The virtual block device should appear as:
/dev/secure_ram

Verify it:
ls -l /dev/secure_ram
lsblk

Create an ext4 Filesystem
Create a filesystem on the virtual device:
sudo mkfs.ext4 /dev/secure_ram

Create a mount point:
sudo mkdir -p /mnt

Mount the device:
sudo mount /dev/secure_ram /mnt

The virtual storage can now be used like a normal filesystem.
Example:
echo "Hello from the encrypted RAM disk" | sudo tee /mnt/test.txt

Read it back:
cat /mnt/test.txt

Encryption Verification
The project was tested using known plaintext and direct inspection of the RAM-backed storage.
Example plaintext:
CAPSTONE_SECRET_123456789

The normal device interface returns the original plaintext after transparent decryption.
The raw backing storage, however, contains ciphertext.
During development, the backing buffer can be inspected through:
/sys/kernel/debug/secure_ram/raw_storage

Example inspection:
sudo dd if=/sys/kernel/debug/secure_ram/raw_storage \
    bs=512 count=1 status=none | xxd

The plaintext does not appear directly in the backing storage.
The corresponding normal read through /dev/secure_ram returns the original plaintext.
This demonstrates:
Application View:
CAPSTONE_SECRET_123456789

RAM View:
Encrypted ciphertext

Functional Testing
The driver was validated using several tests.
Raw Block Read/Write
A known string was written directly to the block device and read back.
Expected behavior:
WRITE → encrypted storage
READ  → decrypted original data

ext4 Filesystem Test
The device was successfully:
formatted with ext4
        ↓
mounted
        ↓
used for normal file operations

Files could be created, read, copied and removed through the mounted filesystem.
Large File Round Trip
A 4 MiB random binary file was:
written to the RAM filesystem
        ↓
copied back
        ↓
SHA-256 compared

The source and recovered files produced identical SHA-256 hashes.
Sector Boundary Testing
The implementation can be tested using data sizes around sector boundaries, including:
511 bytes
512 bytes
513 bytes
1023 bytes
1024 bytes
1025 bytes
4095 bytes
4096 bytes
4097 bytes

This verifies handling of requests that cross sector boundaries.
Ciphertext Inspection
The backing RAM buffer was inspected directly through the debugfs interface.
The stored representation was ciphertext rather than the known plaintext.
Performance Testing
The encrypted RAM disk was benchmarked against the conventional filesystem inside the development VM.
Example measurements obtained during testing:
Workload	Conventional VM Filesystem	Encrypted RAM Disk
Sequential Write	~22.2 MiB/s	~190 MiB/s
Sequential Read	~29.2 MiB/s	~250 MiB/s


These measurements are specific to the development VM and benchmark configuration and should not be interpreted as universal hardware-performance results.
The encrypted RAM disk achieved substantially higher throughput in the tested workload, while encryption introduces additional CPU work.
Benchmarking was performed using fio.
Data Volatility
The storage is intentionally volatile.
The project uses RAM as its backing store rather than persistent storage.
Therefore:
VM running
    ↓
/dev/secure_ram exists
    ↓
RAM contains filesystem data

After shutdown or reboot:
RAM-backed contents disappear
/dev/secure_ram is no longer present
until the kernel module is loaded again

The project source code and compiled module stored on the VM's persistent virtual disk remain available.
Security Considerations
The encryption subsystem protects the confidentiality of data stored in the RAM-backed block storage.
The project uses AES-XTS through the Linux Kernel Crypto API rather than implementing AES manually.
However, the current implementation should be considered an academic prototype rather than production-grade encrypted storage.
Important limitations include:
- The current encryption key is ephemeral and exists only while the driver is active.
- Reloading the driver generates a new key.
- Existing encrypted contents therefore cannot be recovered after the key is destroyed.
- AES-XTS provides confidentiality but does not by itself provide authentication/integrity protection against ciphertext modification.
- The project does not claim to encrypt every copy of a file that may exist elsewhere in system memory, caches, or application memory.
- Key management is intentionally simplified for the capstone implementation.
Advantages
- Very low storage latency compared with the tested VM filesystem.
- Temporary storage that does not persist across shutdown.
- Transparent integration with Linux filesystem operations.
- Encryption is handled inside the storage path.
- Demonstrates kernel programming, block I/O, filesystem integration and cryptography in one project.
Limitations
- Storage capacity is currently limited to 16 MiB.
- Data is lost when the RAM-backed storage is destroyed.
- The current project is a prototype and is not intended to replace mature storage-encryption systems.
- Key management is simplified.
- XTS does not provide authenticated integrity.
- Performance results depend heavily on the host machine, VM configuration and workload.
Future Enhancements
Possible extensions include:
- Configurable RAM-disk size.
- Improved key management.
- User-configurable keys supplied through a secure interface.
- Encryption-key rotation.
- Integrity/authentication mechanisms.
- Additional block-device statistics.
- More sophisticated concurrency and queue management.
- Configurable sector/block sizes.
- Automated test suites.
- Extended benchmarking across multiple workloads.
- Support for additional filesystem configurations.
Project Status
Current implementation status:
Kernel module                     ✅
RAM-backed storage                 ✅
Virtual block device               ✅
/dev/secure_ram                    ✅
BIO-based read/write handling      ✅
ext4 filesystem integration        ✅
AES-XTS encryption                 ✅
Transparent decryption             ✅
Ciphertext verification            ✅
Large-file integrity testing      ✅
Performance benchmarking           ✅
Documentation                      🔄

Repository Structure
Encrypted-Virtual-RAM-Disk-Block-Driver/
│
├── secure_ram.c
├── Makefile
├── .gitignore
├── README.md
│
├── docs/
│   └── architecture/
│
├── tests/
│
├── results/
│
└── screenshots/

Disclaimer
This project is developed as an academic Linux systems and storage capstone.
It demonstrates the principles of:
- Linux kernel module development
- Virtual block devices
- RAM-backed storage
- Filesystem integration
- Transparent storage encryption
- Kernel cryptographic APIs
- Storage benchmarking
It should not be considered a production replacement for established Linux storage-encryption solutions.
Author
Abhijeet Kumar Kuila
