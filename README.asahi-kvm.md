# aquarat/qemu-reims-vgpu: macOS guests under KVM on Asahi Linux

This fork's `master` is **upstream QEMU** with two more lines of work merged
in. Nothing here is submitted upstream.

| layer | source | how it arrives |
|---|---|---|
| upstream QEMU | `https://gitlab.com/qemu-project/qemu.git` `master` | `git merge` |
| VMApple + Reims paravirtual GPU device | `steelbrain/qemu-reims-vgpu` `host-reims-vgpu-vmapple` | `git merge` |
| Asahi Linux KVM host support | commits on this `master` | direct commits |

## The GPU: Reims out of process

This QEMU contains no GPU device model. vmapple's GPU slot
(`-M vmapple,gfx-device=apv-gpu-proxy`) holds `apv-gpu-proxy`, a small device
that forwards the paravirtual GPU's registers, interrupts and guest memory to a
separate program, `reims-vgpu-backend` from aquarat/reims-vgpu, over a unix
socket, a shared memfd and eventfds
(`include/hw/display/apv-gpu-proxy-protocol.h`, MIT). Nothing from Reims,
metal2vulkan, Rust or Vulkan is built into or loaded by this QEMU. A crash in
the host GPU driver ends only the backend: the device stops the guest with the
reason (`on-backend-loss=stop`, the default), or resets it onto a fresh
backend (`reset`).

```sh
reims-vgpu-backend --socket /run/gpu.sock &
qemu-system-aarch64 -M vmapple,gfx-device=apv-gpu-proxy,aux-ram-share=on \
    -global apv-gpu-proxy.socket=/run/gpu.sock \
    -object memory-backend-memfd,id=ram,size=8G,share=on -M memory-backend=ram ...
```

Guest RAM must be fd-backed and shared (`memory-backend-memfd,share=on`,
`aux-ram-share=on`) so the backend can map it. The experiment repository's
`launch-kvm.sh` does all of this when `GPU_BACKEND_BIN` is set.

The in-process Reims device (`reims-vgpu-mmio`, Reims linked into QEMU as a
Rust staticlib) is no longer in this branch. aquarat/reims-vgpu still builds it
from its `vendor/qemu` submodule, which pins the last commit of this repository
that has it (6740a49344).

## Code from other authors

- **vmapple GICv2m MSI frame** (`hw/vmapple/vmapple.c` `create_gicv2m`, the
  `macos-compat` property of `hw/intc/arm_gicv2m.c`), the apple-gfx
  PGMemoryMapDescriptor changes, the AES `MAX_LEN` change and
  `CONFIG_VMAPPLE=y`: Mohamed Mediouni's qemu-devel RFC v7 series
  "vmapple: make it work on the latest macOS release", patches 1–6, applied
  by Anees Iqbal in e181197cc2.
- **The original vmapple KVM work** (Anees Iqbal, 7f39007f9a) is rewritten:
  `target/arm/kvm-vmapple.c` serves the platform hypercalls and the vmapple
  machine wires the CPUs, PSCI and the register helper itself
  (5deca34b77, c9bd4dfb8f). His HVF-only changes (vGIC bases, NO_RAW cpregs,
  ISV=0 aborts) are reverted, so `target/arm/hvf/` is upstream's.
- **Left as they are:** the macOS-host-only `hw/display/apple-gfx*.m`
  (upstream's, plus the RFC v7 changes above). They are not built on Linux.

The KVM host commits on top of steelbrain's branch (the history; some are
superseded as described above):

- `vmapple: run macOS guests with KVM on Asahi Linux hosts` (Anees Iqbal):
  KVM wiring, Apple's private PAuth HVC service, PSCI 1.1, Linux `map_pages`.
- `hw/vmapple/bdif`: report the real root/aux disk sizes (not a fixed 64 GiB).
- `target/arm/kvm`: return the VMApple PAC HVC status in x0 (macOS 26).
- `hw/display/reims-vgpu-mmio`: poll the device without a host window (headless).
- `hw/vmapple`: follow upstream's removal of `machines-qom.h`.
- `hw/vmapple`: the `avp,rtc` real-time clock (machine property `avp-rtc`).
- `hw/vmapple`: `gfx-device=none` for guests without a paravirtual GPU.
- `hw/display/reims-vgpu-mmio`: packed page views on Linux (memfd-backed RAM).
- `util`: guest RAM aligned to the host's THP PMD size (32 MiB on 16K hosts).
- `virtio-balloon`: `macos-units` for Apple's AppleVirtIOBalloon driver.
- `virtio-snd`: answer empty JACK_INFO/CHMAP_INFO queries (AppleVirtIOSound).
- `hw/vmapple`: at most `max-instances` (default 2, the macOS licence limit)
  vmapple guests per Linux host; a further QEMU refuses to start.
- `virtio`: relocate a used ring that macOS 26's AppleVirtIO driver places
  inside the available ring (`x-fix-overlapping-used`, on for vmapple); fixes
  ~8 % of boots never reaching the network and ~4 % with no sound device.

- `hw/display`: `apv-gpu-proxy`, the out-of-process GPU device, replacing the
  in-process Reims shims; vmapple's `gfx-device` takes `apple-gfx-mmio`,
  `apv-gpu-proxy` or `none`.

It needs a host kernel with two KVM patches: Apple's PAuth VM-key state and
in-KVM emulation of MMIO loads/stores without a syndrome (`patches/` in the
experiment repository below). Usage, tests and the technical notes are in
aquarat/apple-macos-arm64-vm-guest.

## Taking upstream changes

```sh
git remote add upstream https://gitlab.com/qemu-project/qemu.git
git remote add steelbrain https://github.com/steelbrain/qemu-reims-vgpu.git
git fetch upstream master && git fetch steelbrain host-reims-vgpu-vmapple
git merge steelbrain/host-reims-vgpu-vmapple
git merge upstream/master
```

steelbrain's branch carries the in-process Reims shims this fork removed; if a
merge brings any back (`hw/display/reims-vgpu-*`, the cargo glue in
`hw/display/meson.build`), drop them again.

Then build (`MODE=oop scripts/build-qemu.sh` in the experiment repository)
and run the Ventura/Tahoe job loops before pushing.
