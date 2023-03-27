Non-interpreted execution test for visicalc
===========================================

This tool is meant to support root-cause analysis for the issue reported in
https://retrocomputing.stackexchange.com/questions/25976/visicalc-v1-0-is-not-working-on-modern-computer-as-expected

Current x86 and x64 processors are still able to execute 16-bit code in protected mode
(a type of code execution used in Windows 3.1 standard mode), and visicalc can be partially
executed with a couple of patches in a primitive "virtualized" environment in 16-bit
protected mode. Any processor issue affecting visicalc in real mode is likely to affect
it in 16-bit protected mode, too.

This tool loads vc.com, patches stuff that tries to interact with DOS or the BIOS,
makes sure no segment register stuff disturbs operation (e.g. by making sure there is
just enough memory handed to visicalc that it can't allocate memory for a single data
cell), and then runs visicalc up to the point where the initial screen drawing happens,
and then dumps the screen buffer contents to stdout.

It should work with VC.COM having 27520 bytes.

The tool can be compiled both as 32-bit or 64-bit linux tool. It requires VC.COM in
the current directory when you execute it.
