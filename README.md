# hotpath

## Overview

## Benchmark
We are using two QEMU virtual machines to benchmark the pipeline. The first virtual machine is running PKTGEN and is writing packets
to the MAC address of the second VM. The second VM has two threads running where one thread is constantly consuming from the NIC and
pushing those packets read straight to the SPSC queue. The other thread then pops from the SPSC queue and reads the start time from
those packets to get the **receive-to-consumer handoff latency**: the time from the receive call returning the packet (the receive thread
stamps it right after `rte_eth_rx_burst`/`recv`/`recvmmsg` returns) to the consumer popping it from the queue and the data being usable.
Note this window starts *after* the receive call, so the kernel's own receive work (syscall, copy, wakeup) happens before the stamp and is
not part of the latency numbers (see [What the latency numbers do and don't show](#what-the-latency-numbers-do-and-dont-show)). We use rdtsc
counter to see how many CPU cycles it takes for the DPDK path. The socket implemenations call clock_gettime(CLOCK_MONOTONIC_RAW) directly. 
By taking the inverse of the counters frequency we get the time taken in seconds per cycle.
Multiplying this by 1e9 gives us the time taken in ns. There are three pipelines we are comparing. 
- The one we expect to perform the best is the DPDK kernel bypass pipeline that uses a busy polling approach to read from the NIC. It is using rte_eth_rx_burst to read batches/bursts
of packets from the NIC. These are then pushed as a batch to the SPSC ring and popped from the ring as a batch.
- The next is a simple socket based approach, that uses the recv system function to read from the socket. The single version of this
is reading from the socket one packet at a time and pushing to the ring one at a time. We then pop each item from the ring one at
a time and process it. recv under the hood is **interupt based** because it puts the thread to sleep when you call it. When data
arrives at the NIC a physical CPU interrupt triggers the OS network driver wake the thread and copy the data. 
- The last approach is the same simple socket approach, but we are trying to read from the NIC in batches similar to how rte_eth_rx_burst
reads in bursts. We then push and pop these bursts from the SPSC ring using its push/pop batch functions. 
## Benchmark Results
![Latencies compared across all three pipelines](analysis/latency_comparison.png)
## Analysis
We can see here that the DPDK-based implementation clearly has the best p50 latency and tail latency, and it is the only path that
delivered every offered packet (0% dropped vs. 39–45% for the socket paths). The drop rate and throughput gap is where the kernel bypass
shows up directly: every packet the socket path receives has to go through the kernel stack, which caps how fast it can drain the NIC.
When we use recv or recvmmsg we must do the following steps per call:
1. Switch from userspace to kernel space
2. Kernel grabs file-descriptor for the socket from process file-descriptor table
3. Socket buffer is locked so not corrupted during read
4. Data is copied from kernel's sk_buff to user space pointer
5. Kernel cleans up (free sk_buff, unlock socket, update TCP window sizes)
6. Switch back to usermode from kernel mode
If the buffer is empty after step 3 **additionally**:
1. Thread is descheduled and removed from active run-queue
2. CPU does a full context switch 
3. The thread is woken up and rescheduled once network softirq finishes with packet
4. Resume on step 4 from above

We are able to skip this using DPDK. The NIC driver is running in userspace using vfio, and the packets are transferred from the NIC via direct memory access (DMA) into the 
ring of descriptors fed into the queue created with rte_eth_rx_queue_setup. When we call rte_eth_rx_burst() we aren't performing a syscall, it just reads the descriptor rings
that have already been written by the NIC. We are skipping the interrupt, context switch, copy, and thread descheduling (while waiting for something to come).
This is why DPDK keeps up with the offered load when the sockets can't. There is an inherent tradeoff we see between the recv()/recvmmsg() implementations. They either have a low discovery latency, but
have to pay the cost of the syscall each time OR have less syscalls via batching with the con of earlier packets in each batch being stale. rte_eth_rx_burst doesn't have this
cost to deal with; So it can read from the nic constantly in a busy poll loop to get fast discovery latency, while also picking up many packets per call. It doesn't have to 
choose one.

### What the latency numbers do and don't show
The kernel steps above happen *before* the receive thread stamps the packet, so they are not inside the measured latency window.
The latency gap between DPDK and the sockets (140ns vs ~780ns p50) comes from what happens after the receive call: the hand-off through
the ring and the consumer picking the packet up. Likely contributors are the socket consumer calling `std::this_thread::yield()` (a syscall)
when the ring is empty while the DPDK consumer busy-spins with `rte_pause()`, the cost of `clock_gettime` vs `rdtsc`, and the kernel's
packet processing competing with the consumer for one of the VM's two vCPUs. These haven't been separated out yet.
To measure the kernel receive path itself, the next step is to stamp socket packets with the kernel's own receive timestamp
(`SO_TIMESTAMPNS`) instead of stamping after `recv()` returns, so the measured window includes the syscall, copy and wakeup.

### Packet Loss
Now when we compare the two socket based implementation's we have to note something interesting. When I first ran the numbers with
the recvmmsg implementation of the socket, I was disappointed. It consistently lagged behind the single packet recv implementation in terms
of latency, ie ~100ns on the p50. I was concerned that the batch implementation wasn't showing up as faster because there wasn't enough
traffic reach the VM (wasn't sure if there was some bottleneck somewhere between the two VM's I wasn't aware of). To fix this, I logged the packets dropped
and found that actually we were dropping around 44% of our packets. This means the problem with our batch wasn't that we weren't getting enough packets.
What I first did to lower the amount of packets being dropped, was set the SO_RCVBUFFORCE linux socket option to 8MB, so the socket is allowed to have more
data pile up before its dropped. This succeeded in slightly reducing the percentage dropped 7-8%. When we account for the packets dropped for the single recv socket
numbers we get a different story between the two implementations.

**Batched Packet Loss**

| Period | Received | Dropped  | Percent Dropped (of received + dropped) |
|---|---|----------|-----------------------------------------|
| Before 1 | 491,120 | 390,891  | 44.3%                                   |
| Before 2 | 423,800 | 323,572  | 43.3%                                   |
| Before 3 | 480,754 | 380,531  | 44.2%                                   | 
| Before Avg | — | —  | ~43.9%                                  | 
| After 1 | 304,889 | 187,412  | 38.1%                                   | 
| After 2 | 301,312 | 183,835  | 37.9%                                   |
| After 3 | 243,189 | 125,712  | 34.1%                                   | 
| After Avg | — | — | ~36.7%                                  |  

### recv() vs recvmmsg()
What we see in these numbers from the latency by percentile is a couple of things:
1. recv (single) sees more raw offered packets in absolute terms because it's throughput-limited (one packet per call), so it takes longer in total time to execute
the 95,000 iteration run. This means over the duration of the run, more packets are seen and dropped.
2. Of this greater number of packets sent during the run, the percentage dropped by the recv (single) is more still compared to recvmmsg (batch). We can see that 
around 6% more were delivered by the recvmmsg runs.
3. If we set vlen to 1 in the recvmmsg() parameters we get these numbers: \
vlen=1 batch p50 values (10 runs): 780, 780, 790, 790, 780, 780, 790, 790, 780, 790 → average ≈ 785ns \
vlen=1 batch p99 values: 1140, 1180, 1320, 1300, 1250, 1130, 1130, 1220, 1250, 1130 → average ≈ 1205ns \
Single recv() pooled (n=950,000): p50=780ns, p99=1,180ns \
This is interesting because we can see this pulls the batched number right inline with the single socket p50 and p99 numbers. There is real overhead added by using 
recvmmsg as it has to allocate and copy the mmsghdr struct that recv doesn't. The difference this adds is not detectable with our noise floor.

| Config | Wall-clock duration | recvmmsg calls/run | % calls returning full batch | Max `rx_ring` occupancy (of 4096) |
|---|---|---|---|---|
| Batch, vlen=32 | 0.385s – 2.222s (avg ≈0.59s) | ~3,125 | ~100% (n=32 nearly every call) | 32–64 typical, one run hit 1,632 |
| Batch, vlen=1 | 0.919s – 1.009s (avg ≈0.96s) | 100,000 | 100% (forced, n=1 capped) | 2–45 |
| Single recv() | 0.758s – 0.841s (avg ≈0.80s) | N/A (not recvmmsg) | N/A | 3–52 |

4. When we ran with the number of elements received from each recvmmsg() call we saw that actually basically always we were pulling 32 elements. What this means is that batching
was being fully utilized but vlen(32) was slower than vlen(1) per packet. That could either the extra cost of the recvmmsg() overhead that scales with vlen or it could be
just an artifact of how we are recording per packet. Even though we are seeing a worse per-packet latency, vlen=32 finishes the run in half the wall clock duration. This is
because there are fewer recvmmsg() calls for the same work, which is the batching tradeoff we make: better aggregate throughput, but worse per packet latency.
5. The p99.9 and max latency numbers are in large millisecond ranges that we also see in the DPDK bench. We have determined they aren't rx_ring occupancy by the table above
as those outliers still exist with rx_ring occupancy very low relative to its total size. This means there's no pressure filling up the rx_ring causing latency spikes. Some of it
we were able to trace to the host CPU scheduling underlying these vCPUs. Running perf sched trace we found a max delay of 2.6ms, which is only part of the 7ms we were tending to see.
The max and p99.9 numbers are being heavily affected by noise in the test environment ie don't have core isolation configured on the host. We have configured the vCPU's threads to only run on those cores, but
we haven't done anything to prevent the OS from scheduling other things its wants on those cores.
For comparing recv, dpdk and recvmmsg, use the p50 and p99 numbers as the reliable comparison.
## Local Setup
**Create the isolated network:**

```bash
cat > pktgen-isolated.xml <<'EOF'
<network>
<name>pktgen-isolated</name>
<bridge name='virbr-pktgen' stp='off' delay='0'/>
</network>
EOF
sudo virsh net-define pktgen-isolated.xml
sudo virsh net-start pktgen-isolated
sudo virsh net-autostart pktgen-isolated
```

This is a private, direct wire without NAT, this is to remove the noise of other things going on my computers default network

### QEMU Setup:

**Install command:**

`sudo pacman -S qemu-full libvirt virt-manager dnsmasq edk2-ovmf
`

**Enable libvrt daemon:**

`sudo systemctl enable --now libvirtd.service
`

Add your user to the libvirt and kvm groups so you can manage VMs and access /dev/kvm without sudo:

`sudo usermod -aG libvirt,kvm $USER
`

**Validate:**

`virt-host-validate qemu
`

**Login:**

**1. From your host terminal (not inside the VM), run:**

```bash
   virt-install \
   --name dpdk-vm \
   --memory 4096 \
   --vcpus 2 \
   --disk size=20 \
   --cdrom /var/lib/libvirt/images/ubuntu-26.04-live-server-amd64.iso \
   --os-variant ubuntu24.04 \
   --network network=default,model=virtio \
   --graphics spice
```
`sudo virsh net-dhcp-leases default
`

**2. Confirm you see an IP for dpdk-vm, then SSH in from the host:**

`ssh <your-username>@<that-ip>
`

**3. Once that SSH session works, close the virt-viewer window**

**Run in the VM:**

```bash
sudo apt update && sudo apt install -y \
git build-essential meson ninja-build python3-pyelftools \
libnuma-dev pkg-config
```
**What each piece is for:**

- build-essential — gcc/g++/make, the basic C/C++ toolchain DPDK needs to compile.
- meson + ninja-build — DPDK switched its build system to Meson+Ninja years ago (no more make config && make like old versions); this is what actually drives the DPDK build.
- python3-pyelftools — DPDK's build scripts use this to inspect compiled binaries (symbol/ELF section analysis) as part of the build process.
- libnuma-dev — DPDK is NUMA-aware (it cares which memory node a core's traffic lives on) even though your VM is single-socket/no real NUMA; the library headers are still a hard build dependency.
- pkg-config — standard Linux convention for libraries to advertise their compile/link flags; DPDK both consumes and provides .pc files.
- git — to actually clone the DPDK source repo, which we'll do next.

**Allocate the hugepages**

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
grep Huge /proc/meminfo
```

Attach the second NIC:

```bash
virsh -c qemu:///system attach-interface \
  --domain dpdk-vm \
  --type network \
  --source pktgen-isolated \
  --model virtio \
  --config --live
```
**Then rebind the NIC:**
```bash
cd ~/dpdk
sudo modprobe vfio_pci
echo Y | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
sudo ./usertools/dpdk-devbind.py --bind=vfio-pci 0000:07:00.0 --force
./usertools/dpdk-devbind.py --status
```
**Build DPDK:**
```bash
git clone https://github.com/DPDK/dpdk.git ~/dpdk
cd ~/dpdk
meson setup builddir
ninja -C builddir
sudo ninja -C builddir install
sudo ldconfig
```

### Pktgen VM Setup

1. Create pktgen-vm:
   ```bash
    virt-install \
    --name pktgen-vm \
    --memory 4096 \
    --vcpus 2 \
    --disk size=20 \
    --cdrom /var/lib/libvirt/images/ubuntu-26.04-live-server-amd64.iso \
    --os-variant ubuntu24.04 \
    --network network=default,model=virtio \
    --graphics spice

    sudo virsh net-dhcp-leases default
    ```
2. Attach the second NIC:

    ```bash
    virsh -c qemu:///system attach-interface \
   --domain pktgen-vm \
   --type network \
   --source pktgen-isolated \
   --model virtio \
   --config --live
    ```
3. SSH in, then install the same DPDK build deps as dpdk-vm:

    ```bash
   sudo apt update && sudo apt install -y \
   git build-essential meson ninja-build python3-pyelftools \
   libnuma-dev pkg-config
    ```
4. Allocate hugepages (identical to dpdk-vm):

    ```bash
   echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
   grep Huge /proc/meminfo
    ```
5. Build DPDK 
    ```bash
   git clone https://github.com/DPDK/dpdk.git ~/dpdk
   cd ~/dpdk
   meson setup builddir
   ninja -C builddir
   sudo ninja -C builddir install
   sudo ldconfig
    ```
6. Clone and build Pktgen-DPDK: 
    ```bash
   git clone https://github.com/pktgen/Pktgen-DPDK.git ~/Pktgen-DPDK
   cd ~/Pktgen-DPDK
   meson setup builddir
   ninja -C builddir
    ```
7. Bind the NIC to vfio-pci (same as dpdk-vm):
    ```bash
   sudo modprobe vfio_pci
   echo Y | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
   cd ~/dpdk
   sudo ./usertools/dpdk-devbind.py --bind=vfio-pci 0000:07:00.0 --force
   ./usertools/dpdk-devbind.py --status
    ```
8. Run it:
    ```bash
   cd ~/Pktgen-DPDK
   sudo ./builddir/app/pktgen -l 0,1 -n 4 -a 0000:07:00.0 -- -P -T -m "1.0"
    ```
   
**In DPDK VM Clone hotpath:**

```bash
git clone https://github.com/jamesn14/hotpath ~/hotpath
```
Build rx_bench:

```bash
cd ~/hotpath/dpdk_rx_app
cmake -B cmake-build-dpdk-vm
cmake --build cmake-build-dpdk-vm --target rx_bench
```
Build socket_bench:

```bash
cd ~/hotpath/socket_rx_app
cmake -B cmake-build-dpdk-vm
cmake --build cmake-build-dpdk-vm --target socket_bench
```
`rx_bench` needs the NIC on `vfio-pci`:

```bash
sudo dpdk-devbind.py --bind=vfio-pci 0000:07:00.0
cd ~/hotpath/dpdk_rx_app/cmake-build-dpdk-vm
sudo ./rx_bench -l 0,1 -n 4 -a 0000:07:00.0 -- 5000 100000
```

`socket_bench` needs the NIC on `virtio-pci`:
```bash
sudo dpdk-devbind.py --bind=virtio-pci 0000:07:00.0
cd ~/hotpath/socket_rx_app/cmake-build-dpdk-vm
sudo ./socket_bench enp7s0 5000 100000 single
sudo ./socket_bench enp7s0 5000 100000 batch
```
If you want to run the versions that pool over 10 runs do use these instead
```bash
~/hotpath/scripts/run_repeated.sh 10 rx_bench_latencies.csv runs/rx_bench -- sudo ./rx_bench -l 0,1 -n 4 -a 0000:07:00.0 -- 5000 100000
```
```bash
~/hotpath/scripts/run_repeated.sh 10 socket_bench_batch_latencies.csv runs/socket_batch -- sudo ./socket_bench enp7s0 5000 100000 batch
```
```bash
~/hotpath/scripts/run_repeated.sh 10 socket_bench_single_latencies.csv runs/socket_single -- sudo ./socket_bench enp7s0 5000 100000 single
```
If you do that pull the csv files into analysis/runs and run `python3 plot_latencies.py`