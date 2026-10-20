# KernelSU-Next driver patches

The `msm8998-oneplus-k` kernel builds the KernelSU-Next driver from an external
checkout that the tree reaches through the gitignored `drivers/kernelsu`
symlink, so driver sources are not committed here. What the tree does require
is the glue below: the SUSFS integration the driver lacks at the pinned commit.

## Provenance

- Driver: <https://github.com/KernelSU-Next/KernelSU-Next>, pinned at
  `465490a26667ba4cf00b3be975a7e28e53978829`. That commit is the parent of the
  one that raised the supercall UAPI to 6, so it declares UAPI 5 and pairs with
  the released `v3.4.2` manager the device runs. Always clone the canonical
  repository, never a fork.
- Glue: `0001-susfs-glue.patch` - 7 files, +202 lines, 0 deletions: the SUSFS
  Kconfig block, the init and setuid hooks, the SID caches, and the supercall
  dispatch. No other driver file is touched.
- SUSFS host implementation: the `susfs: backport v2.3.0 kernel-side support
  for 4.19` commit of this branch. It is a third-party adaptation for non-GKI
  kernels, adapted from `JackA1ltman/NonGKI_Kernel_Build_2nd` at `dcd7896`,
  file `Patches/Patch/susfs_patch_to_4.19.patch`; as that commit records, it
  is "not a simonpunk non-GKI release and not an upstream-certified pairing
  with KernelSU-Next". The canonical SUSFS project source is
  <https://github.com/simonpunk/susfs4ksu>, whose `kernel-4.19` branch stops at
  v1.5.5 and cannot supply the v2 API.

## Why the host adaptation is a prerequisite

The glue calls the SUSFS v2 host API (`susfs_init`, `susfs_extra_works`,
`susfs_start_sdcard_monitor_fn`, `susfs_add_sus_path_loop`, `SUSFS_MAGIC`,
`susfs_enable_log`). Only the v2.3.0 backport in this tree defines those, so a
tree without it cannot link the glued driver; the patch is meaningful only
together with that commit.

## Preparation

Run from the kernel tree root, on a clean tree:

```sh
git clone https://github.com/KernelSU-Next/KernelSU-Next.git KernelSU-Next
git -C KernelSU-Next checkout --detach 465490a26667ba4cf00b3be975a7e28e53978829
git -C KernelSU-Next rev-parse HEAD          # must print the pin
git -C KernelSU-Next status --porcelain      # must print nothing
cd patches/kernelsu-next && sha256sum -c SHA256SUMS && cd ../..
git -C KernelSU-Next apply --check "$PWD/patches/kernelsu-next/0001-susfs-glue.patch"
git -C KernelSU-Next apply "$PWD/patches/kernelsu-next/0001-susfs-glue.patch"
ln -s ../KernelSU-Next/kernel drivers/kernelsu
~/scripts/build_kernel -s msm8998-4-19 -b msm8998-oneplus-k -k -p
```

The sanctioned build does not apply the glue itself: `build_kernel` only adds
`KSU_DEFINED=1` to the make variables and never clones, resets, or patches the
driver checkout, so a prepared checkout is an input to the build, not something
the build produces. Rerunning preparation requires a fresh clean checkout -
`git clone` into an existing directory fails and `git apply` refuses an
already-patched tree, so remove `KernelSU-Next/` first rather than reusing it.
