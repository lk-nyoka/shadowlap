# Setup on Windows 11

QNX documents its QEMU route for Ubuntu, so on Windows 11 we use Ubuntu inside
WSL2. Everything here is free. Steps marked **check** depend on your machine or
on QNX tooling I could not run, so confirm them as you go.

## Part 1: run the project without QNX (about 20 minutes)

This proves the code works on your machine before you touch QNX.

1. Open **PowerShell as administrator** and run:
   ```powershell
   wsl --install -d Ubuntu-24.04
   ```
   Restart when asked, then open "Ubuntu 24.04" from the Start menu and create
   a Linux username and password.

2. In the Ubuntu window:
   ```bash
   sudo apt update
   sudo apt install -y build-essential git python3-venv python3-pip unzip
   ```

3. Get the project and run it:
   ```bash
   cd ~
   # either clone the GitHub repo:
   #   git clone https://github.com/<you>/shadowlap.git
   # or copy the zip from a Windows folder (keep the quotes if the path has spaces):
   #   cp "/mnt/c/Users/<you>/Downloads/shadowlap.zip" ~ && unzip -o shadowlap.zip
   cd shadowlap
   make && make test
   python3 -m venv .venv && . .venv/bin/activate
   pip install numpy matplotlib
   python3 tools/make_synthetic_lap.py
   SECONDS_MAX=30 sh scripts/run_demo.sh shadow out
   ```
   A full lap takes about 90 seconds per scenario;
   `sh scripts/run_demo.sh all out` runs all seven (about 11 minutes).

   **Expect some v1 deadline misses and host stalls on Windows.** Ubuntu in WSL
   is a virtual machine, and Windows can pause it for several milliseconds at a
   time. On a Windows 11 laptop we measured 40 v1 misses and 82 host stalls in
   one lap, even with `sudo` (real-time priority) and nothing misbehaving. That
   is the problem a real-time OS removes, and why the QNX runs are the ones to
   quote. Run with `sudo` for real-time priority inside the VM.

4. **Watch it live.** Open a second Ubuntu window:
   ```bash
   cd ~/shadowlap && . .venv/bin/activate
   python3 tools/live.py out
   ```
   Open http://localhost:8000 in your Windows browser (WSL passes it through),
   then start a run in the first window: `sh scripts/run_demo.sh all out`.

5. Make the report and open it in your Windows browser:
   ```bash
   python3 tools/report.py out
   explorer.exe "$(wslpath -w out/report.html)"
   ```

## Part 2: QNX in QEMU

6. **Check virtualisation.** QEMU is much faster with KVM:
   ```bash
   ls -l /dev/kvm
   ```
   If it is missing: enable virtualisation (Intel VT-x / AMD-V) in your BIOS,
   then in Windows create `C:\Users\<you>\.wslconfig` containing
   ```ini
   [wsl2]
   nestedVirtualization=true
   ```
   and run `wsl --shutdown` in PowerShell, then reopen Ubuntu. Then:
   ```bash
   sudo usermod -aG kvm $USER     # log out of Ubuntu and back in afterwards
   sudo apt install -y qemu-system-x86
   ```
   QEMU still works without KVM, just slowly.

7. **Get QNX (free, non-commercial).**
   1. Create a myQNX account, then (logged in) open qnx.com/getqnx, accept the
      non-commercial terms and deploy the licence to your email. It then shows
      in the myQNX License Manager.
   2. Download **QNX Software Center for Linux** (`qnx-setup-...-linux.run`)
      from your myQNX account page.
   3. In Ubuntu:
      ```bash
      sudo apt install -y libgtk-3-0t64 libxtst6     # libraries for its window
      sh qnx-setup-*-linux.run                       # press q, then y to accept
      ```
      It installs to `~/qnx/qnxsoftwarecenter` and opens its window on your
      Windows desktop (Windows 11 shows Linux windows from WSL).
   4. Log in, install **QNX Software Development Platform 8.0** to
      `~/qnx800`, then under Manage Installation > Available search
      "quick start" and install **QNX SDP 8.0 Quick Start Target Image for QEMU**.
      Both downloads are large; expect it to take a while.

8. Load the QNX tools in every new terminal:
   ```bash
   source ~/qnx800/qnxsdp-env.sh
   qcc --version
   which mkqnximage
   ```

9. **Build and boot a QNX image.** **Check** `mkqnximage --options` first. It
   lists host packages it needs, and whether you can give the VM 2 CPUs (useful
   for pinning production and shadow to separate cores).
   ```bash
   mkdir -p ~/qnxvm && cd ~/qnxvm
   mkqnximage --type=qemu --run
   ```
   It may ask for `sudo` to set up networking. When QNX boots, log in (the
   default accounts are in the mkqnximage docs; **check** yours) and find its
   address with `ifconfig`.

## Part 3: run the project on QNX

10. Build for QNX on the Ubuntu side:
   ```bash
   cd ~/shadowlap
   source ~/qnx800/qnxsdp-env.sh
   make TARGET=qnx bundle
   ```

11. Copy it to the QNX VM (any writable folder; `/data/home/root` is typical):
    ```bash
    scp -r build/qnx-x86_64/bundle root@<QNX-IP>:/data/home/root/shadowlap
    ```

12. On QNX:
    ```sh
    cd /data/home/root/shadowlap
    sh run_demo.sh shadow out                      # one scenario
    V2_USER=qnxuser sh run_demo.sh attack out      # v2 as a normal user: should be BLOCKED
    PROD_CPU=0 SHADOW_CPU=1 sh run_demo.sh all out # all scenarios, cores split (needs 2 CPUs)
    ```

    To watch a QNX run live on your PC, open two Ubuntu windows first:
    ```bash
    sh tools/follow_qnx.sh <QNX-IP> shadow      # window 1: streams the run
    python3 tools/live.py out-qnx               # window 2: the dashboard
    ```
    then start `sh run_demo.sh shadow out` on QNX.

13. Copy the results back and build the report on Ubuntu:
    ```bash
    scp -r root@<QNX-IP>:/data/home/root/shadowlap/out ./out-qnx
    python3 tools/report.py out-qnx
    ```

## If something fails

- `qcc: command not found`: you skipped `source ~/qnx800/qnxsdp-env.sh`.
- `resmgr_attach ... Permission denied` on QNX: run the plant as root.
- `[v1] warning: SCHED_FIFO ... not set`: run as root (QNX) or with `sudo` (Linux).
- QEMU very slow: no KVM. See step 6.
