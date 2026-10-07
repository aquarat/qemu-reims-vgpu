# aquarat/qemu-reims-vgpu: macOS guests under KVM on Asahi Linux

This fork's `master` is **upstream QEMU** with two more lines of work merged
in. Nothing here is submitted upstream.

| layer | source | how it arrives |
|---|---|---|
| upstream QEMU | `https://gitlab.com/qemu-project/qemu.git` `master` | `git merge` |
| VMApple + Reims paravirtual GPU device | `steelbrain/qemu-reims-vgpu` `host-reims-vgpu-vmapple` | `git merge` |
| Asahi Linux KVM host support | commits on this `master` | direct commits |

The KVM host commits on top of steelbrain's branch:

- `vmapple: run macOS guests with KVM on Asahi Linux hosts` (Anees Iqbal):
  KVM wiring, Apple's private PAuth HVC service, PSCI 1.1, Linux `map_pages`.
- `hw/vmapple/bdif`: report the real root/aux disk sizes (not a fixed 64 GiB).
- `target/arm/kvm`: return the VMApple PAC HVC status in x0 (macOS 26).
- `hw/display/reims-vgpu-mmio`: poll the device without a host window (headless).
- `hw/vmapple`: follow upstream's removal of `machines-qom.h`.
- `hw/vmapple`: the `avp,rtc` real-time clock (machine property `avp-rtc`).

It needs a host kernel with two KVM patches: Apple's PAuth VM-key state and
in-KVM emulation of MMIO loads/stores without a syndrome (`patches/` in the
experiment repository below). The Reims device (aquarat/reims-vgpu) vendors this
repository as `vendor/qemu` and builds QEMU from there. Usage, tests and the
technical notes are in aquarat/experiment-macos-arm64-on-asahi-linux-arm64.

## Taking upstream changes

```sh
git remote add upstream https://gitlab.com/qemu-project/qemu.git
git remote add steelbrain https://github.com/steelbrain/qemu-reims-vgpu.git
git fetch upstream master && git fetch steelbrain host-reims-vgpu-vmapple
git merge steelbrain/host-reims-vgpu-vmapple
git merge upstream/master
```

Then build through aquarat/reims-vgpu and run the Ventura/Tahoe job loops
before pushing. `scripts/sync-upstream.sh` in the experiment repository does
all of this, then bumps the Reims submodule.
