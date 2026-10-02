# retrobox: PS2SDK's smbman (iop/network/smbman @ b12f8af), vendored so directory listings can fetch entries in
# batches -- stock smbman asks the server for one entry per FIND_FIRST2/FIND_NEXT2 round trip.
IOP_BIN  = smbman.irx
IOP_OBJS = smbman.o smb_fio.o smb.o poll.o auth.o des.o md4.o imports.o

IOP_LIBS += -lgcc

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
