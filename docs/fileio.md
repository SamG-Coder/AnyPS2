# Disc-backed file-I/O RPC

`nt_fileio` owns the legacy FILEIO service and its file handles. It reads bounded
ranges through `nt_disc::Iso9660`. The SIF layer owns packet decoding, RPC
correlation and delivery; the service does not know command-buffer addresses or
guest code entry points.

## Mount and interface

Pass `--disc=IMAGE.iso` to the generated native probe, or configure
`ANYPS2_TEST_DISC` for the local startup CTest. The image is opened read-only.
Mounting must happen before SIF command initialization. Without a mount, binding
to FILEIO continues to return a null server.

The mounted endpoint binds RPC server `0x80000001` and implements the legacy
`1000` protocol: version query (255), open (0), close (1), read (2), and seek (4).
Its IOP server handle and request buffers are private HLE allocations. No BIOS,
IOP module or console filesystem is loaded.

Only `cdrom:` and `cdrom0:` paths resolve against the ISO. File names use the
existing ISO case/version normalization. Traversal and host device paths are
rejected. The service never opens a host path supplied by guest code. Open is
read-only; mutation requests return a read-only error. Missing files, invalid
handles, directories and invalid seeks return negative errors. Malformed discs
and unsupported protocol operations remain explicit failures.

There are at most 64 open files. Reads are limited to 1 MiB per RPC and allocate
only the requested slice, even when a disc file is much larger. Seek supports
start, current and end origins with signed 32-bit results. Reads at or past EOF
return zero; short reads return their actual byte count.

## Completion ordering

Incoming arguments and command packets use the existing SIF1/HLE queue.
HLE submissions can read arguments from EE main RAM or the standard 16 KiB
scratchpad at 0x70000000. Scratchpad DMA channel emulation is separate and remains
unsupported. Outgoing data, read fixups, results and completion packets use the
bounded SIF0 FIFO.
Intermediate destination tags continue the chain; only the final completion
packet terminates it and raises the completion interrupt.

Partial quadwords preserve neighbouring guest bytes. Overlapping output regions
are merged in service-write order. The HLE read writes unaligned edges directly
through these quadwords, then reports zero prefix/suffix fixup sizes to the guest
read callback. It does not infer that an interrupt means the guest has consumed
the reply.

## Current limits and verification

Directory enumeration, stat/ioctl, alternate FILEIO protocol versions, fileXio,
host devices, asynchronous response modes, and other RPC servers are not
implemented. The service is a synchronous HLE endpoint with queued DMA delivery,
not an IOP CPU or a cycle-accurate model.

The `disc-fileio-rpc` test builds an original synthetic ISO and exercises binding,
version negotiation, open/read/seek/close, unaligned multi-sector data, EOF,
result correlation, DMA completion ordering, invalid destinations, path isolation
and read-only errors. It shares ISO fixture builders with the format tests.
Commercial images and all generated game output remain outside the repository.
