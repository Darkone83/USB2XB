# USB2XB

<div align=center>

<img src="https://github.com/Darkone83/USB2XB/blob/main/images/Logo.png" width=375> <img src="https://github.com/Darkone83/USB2XB/blob/main/images/Darkone83.png" width=375>

<img src="https://github.com/Darkone83/USB2XB/blob/main/images/Screenshot.jpg" width=800>

</div>

USB2XB is a dual-pane file manager for the Original Xbox designed to move files directly between the Xbox and standard **FAT32 USB mass-storage devices** connected through a controller-port USB adapter.

It is intended to make USB ↔ Xbox file management possible without FTP, a network connection, or a PC-side Xbox filesystem tool.

> **Release Candidate 2:** USB2XB is still RC software. Keep backups of anything important, especially while testing a new USB device.

## Features

- Dual-pane **USB / Xbox** file browser
- Copy and move files or folders between USB and Xbox storage
- Multi-select support
- Create folders
- Rename files and folders
- Delete files and folders
- FAT32 USB formatting
- USB hotplug after the initial session scan
- Free / total storage display
- Destination free-space preflight before copy begins
- Sort by **Name, Size, or Date**
- Current-directory filename filtering
- XBE metadata display
- Launch XBEs from FATX or USB
- Read-only text viewer
- Read-only hex viewer
- CRC32 and MD5 checksums
- Multi-sector FAT32 transfers optimized for the Original Xbox USB controller
- USB Mass Storage / BOT recovery for transport errors
- Controller-friendly interface with copy progress and conflict handling

## Requirements

You will need:

- A modded Original Xbox capable of running homebrew applications
- An Xbox controller-port to USB adapter
- A compatible USB mass-storage device
- A USB drive prepared with an **MBR partition table and FAT32 partition**

USB2XB currently supports **FAT32** on the USB side. exFAT and NTFS are not supported.

## Important: USB Compatibility

The Original Xbox can be very selective about USB flash drives.

This is not unique to USB2XB. USB compatibility has been well documented for years by people using standard USB storage devices as Original Xbox Memory Units. A USB drive working correctly on a PC does **not** guarantee that it will work correctly on an Xbox.

Some devices may:

- Not be detected at all
- Fail during enumeration
- Connect but behave unreliably during transfers
- Work on one Xbox or adapter combination but not another

If a USB drive does not work correctly, **try another drive**. Community Original Xbox Memory Unit compatibility lists can be useful as a starting point when choosing hardware.

Do not trust an untested USB drive with your only copy of important files.

> **Do not use the stock Microsoft Dashboard to prepare a USB2XB drive.** If the dashboard recognizes a USB device as a Memory Unit, it may format the device as FATX. USB2XB expects FAT32.

## Preparing a USB Drive

For best results:

1. Back up anything important from the USB drive.
2. Connect the drive to a PC.
3. Use an **MBR** partition table.
4. Create a FAT32 partition.
5. Safely eject the drive.
6. Connect it to the Xbox through a controller-port USB adapter.
7. Launch USB2XB.
8. With the USB pane active, press **START** to scan the USB device.

Once USB2XB reports that the FAT32 device is ready, the USB pane can be used normally.

### Format USB

USB2XB includes a **Format USB** option in the USB Actions menu.

This reformats the currently recognized FAT32 volume and **erases its contents**. It is not a full partitioning tool, so a drive that does not already have a usable partition layout should be prepared on a PC first.

## Starting USB / Hotplug

USB access is deliberately started manually to avoid conflicts with the Original Xbox USB stack during application startup.

When USB2XB first launches:

1. Insert the USB drive.
2. Make the **USB** pane active.
3. Press **START**.
4. Wait for USB2XB to report that the FAT32 device is ready.

That first scan enables USB handling for the rest of the session.

After the initial scan, physical hotplug is automatic while USB2XB is idle:

- Remove the USB drive
- Insert it again
- USB2XB will detect and remount it automatically

If needed, **Rescan USB** is available from the USB Actions menu.

> **Do not remove the USB drive while a copy, move, format, or other write operation is in progress.**

## Controls

| Control | Action |
| --- | --- |
| D-Pad Up / Down | Move through the current file list or menu |
| Left Stick Up / Down | Move through the current file list or menu |
| D-Pad Left / Right | Switch panes; while **Sort** is highlighted, change sort mode |
| Left Stick Left / Right | Switch panes; while **Sort** is highlighted, change sort mode |
| LT / RT | Page up / page down |
| A | Open folder, open supported text file, launch XBE, or confirm |
| B | Go up one folder / back |
| Y | Select / unselect item |
| White | Copy / paste staged copy or move |
| Black | Stage a move |
| X | Open Actions menu |
| START | First USB scan; afterward show / hide pane details |
| BACK | Cancel the current operation or open the exit confirmation |

### Viewer Controls

The text, hex, and checksum utilities are read-only.

| Control | Action |
| --- | --- |
| D-Pad Up / Down | Scroll one line / row |
| LT / RT | Page up / page down |
| B / BACK | Exit viewer or cancel an active checksum |

## Copying Files

USB2XB uses a source-and-destination workflow.

### Copy

1. Highlight a file or folder.
2. Optionally press **Y** to select multiple items.
3. Press **WHITE** to stage the copy.
4. USB2XB switches focus to the destination pane.
5. Navigate to the destination folder.
6. Press **WHITE** again to begin the copy.

### Move

1. Highlight a file or folder.
2. Optionally select multiple items with **Y**.
3. Press **BLACK** to stage the move.
4. Navigate to the destination.
5. Press **WHITE** to begin the move.

While choosing a destination:

- **A** opens a folder
- **B** goes up one folder
- **WHITE** starts the transfer
- **BACK** cancels the staged operation

### Free-Space Preflight

During copy preparation, USB2XB expands the selected files and folders once, counts the files, and totals the source file bytes.

Before payload transfer begins, USB2XB checks the destination free space when that information is available.

If the required file bytes exceed the known free space, the copy is stopped before the first payload file is written.

For FAT32 devices whose FSInfo free-space count is unavailable, USB2XB reports the total capacity but does not invent a free-space value or incorrectly block the copy.

## File Conflicts

If a file already exists at the destination:

| Control | Action |
| --- | --- |
| A | Overwrite |
| X | Skip |
| B / BACK | Cancel |

## Actions Menu

Press **X** to open the Actions menu.

### USB Pane

- New Folder
- Rename
- Delete
- Hex Viewer
- Checksums
- Filter
- Rescan USB
- Mount / Unmount USB
- Format USB
- Sort: Name / Size / Date

### Xbox Pane

- New Folder
- Rename
- Delete
- Hex Viewer
- Checksums
- Filter
- Sort: Name / Size / Date

### Sorting

Highlight the **Sort** row and use **Left / Right** to change the value.

Available modes:

- **Name** — alphabetical
- **Size** — largest files first
- **Date** — newest first

Directories remain grouped above files. Selection is preserved where possible when the sort order changes.

Pressing **A** on the Sort row does not change the sort mode; USB2XB will remind you to use **Left / Right**.

### Filtering

Choose **Filter** to open the on-screen keyboard.

- Matching is case-insensitive
- Matching is substring-based
- Files and folders both participate
- The current sort mode is applied after filtering
- Submitting an empty filter clears it
- Changing directories clears the active filter
- The menu displays **Filter: Active** while a filter is in use

Filtering is limited to the current directory; it is not a recursive search.

## Pane Details

After the first USB scan, press **START** to show or hide pane details.

The details view can show information such as:

- Selected filename
- File / folder type
- File size
- Storage free / total capacity
- XBE metadata when an XBE is highlighted

Long metadata values use marquee scrolling where needed.

## XBE Support

### XBE Metadata

When an XBE is highlighted with pane details visible, USB2XB can display:

- XBE title
- Title ID
- Region
- InitFlags
- Disc / version
- File size

Long Region and version values use marquee scrolling when they exceed the available width.

### Launching from Xbox Storage

Highlight an XBE on FATX and press **A**.

USB2XB remaps `D:` to the XBE's containing directory before launching so applications that expect their own files relative to `D:` can start normally.

### Launching from USB

Highlight an XBE on USB and press **A**.

Because an Xbox application cannot execute directly from FAT32 USB storage, USB2XB stages the selected application's containing directory into its private Xbox-side staging area, then launches the staged XBE.

This allows applications that rely on additional files beside the XBE to launch with their supporting content.

## Text Viewer

Press **A** on a supported text file to open the read-only text viewer.

Supported extensions:

- `.txt`
- `.csv`
- `.cfg`
- `.ini`
- `.log`
- `.ver`

Features:

- Up to 512 KB loaded for viewing
- 80-character line wrapping
- D-pad line scrolling
- LT / RT page scrolling
- Truncation indication for oversized files

## Hex Viewer

Choose **Hex Viewer** from the Actions menu for any regular file.

The viewer displays:

- 8-digit file offset
- 16 bytes per row
- Two 8-byte hexadecimal groups
- ASCII representation

Features:

- D-pad row scrolling
- LT / RT page scrolling
- Up to 1 MB loaded for viewing
- Truncation indication for larger files
- Read-only operation

## Checksums

Choose **Checksums** from the Actions menu for any regular file.

USB2XB calculates:

- **CRC32**
- **MD5**

Both hashes are calculated in one streaming pass through the storage abstraction.

Checksum processing is incremental so the UI remains responsive during large files, and **B / BACK** can cancel the operation.

## Transfer Speeds

The Original Xbox controller ports operate at **USB 1.1 Full Speed**.

That means the bus has a theoretical maximum signaling rate of **12 Mbit/s (1.5 MB/s)**. Actual file-transfer speeds are lower because the USB Mass Storage protocol, SCSI/BOT commands, FAT32 filesystem work, Xbox filesystem work, and the USB device itself all add overhead.

USB2XB is optimized to reduce unnecessary transfer overhead, but it cannot turn the Xbox controller ports into USB 2.0.

A USB 2.0 or USB 3.x flash drive will still operate at the speed supported by the Xbox.

In other words: **USB2XB is intended for convenient local file transfer and service work, not high-speed storage.**

## Filesystem Notes

### USB

- FAT32 only
- MBR partitioning recommended
- FAT32 has a maximum individual file size of less than 4 GiB
- Existing FAT32 filenames still need to be compatible with FATX when copied to Xbox storage

### Xbox

USB2XB accesses the Xbox filesystem normally through the console and can browse available Xbox storage volumes from the Xbox pane.

FATX filenames are subject to the Xbox filesystem's naming restrictions, including the 42-byte filename limit.

## Current RC2 Notes

USB2XB RC2 is intended to be feature-complete enough for release-candidate testing rather than continued major feature expansion.

Current areas worth testing heavily include:

- Repeated USB remove / reinsert cycles while idle
- Large USB ↔ FATX transfers
- Large directory trees
- Nearly-full destination volumes
- Long filenames and deep directory paths
- Sort / filter interactions
- Viewer and checksum exit / cancel behavior
- 64 MB retail Xbox memory usage during extended sessions

The browser currently keeps a bounded in-memory directory listing, so exceptionally large directories may not display every entry at once.

Free-space preflight compares logical source file bytes. Very large collections of tiny files can consume additional physical space because of filesystem cluster allocation and directory metadata.

## Troubleshooting

### USB drive is not detected

- Make sure the USB pane is active and press **START** for the initial scan.
- Confirm the drive is FAT32.
- Confirm the drive uses an MBR partition table.
- Disconnect and reconnect the drive.
- Try **X → Rescan USB**.
- Try a different USB flash drive.

USB device compatibility varies considerably on Original Xbox hardware.

### USB is detected but FAT32 does not mount

Recheck the partition and filesystem on a PC. USB2XB expects a valid FAT32 filesystem inside an MBR partition.

### Transfers seem slow

Remember that the Original Xbox is limited to USB 1.1 Full Speed. USB2XB cannot reach modern USB 2.0 or USB 3.x transfer rates.

### A drive works on my PC but not on my Xbox

This is possible and does not necessarily indicate a USB2XB bug. The Original Xbox USB hardware and software stack are known to be selective about USB mass-storage devices.

Try another drive before troubleshooting the rest of the installation.

## Data Safety

USB2XB performs real filesystem writes on both the Xbox and USB device.

Before using a new USB drive:

- Keep backups of important files
- Test the drive with non-critical data first
- Do not disconnect it during writes
- Do not power off the Xbox during a format or active transfer

Once a USB device has proven stable with your Xbox, adapter, and USB2XB, it should provide a convenient way to move and inspect files without relying on a network connection.
