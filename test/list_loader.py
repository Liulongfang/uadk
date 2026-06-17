#!/usr/bin/python3
#-*- coding: utf-8 -*-

import os
import os.path
import sys, getopt
import numpy as np
import subprocess
import tempfile

class listcontent(object):
    def __init__(self, ifile, ofile):
        self.ifile_nm = ifile
        self.ofile_nm = ofile
        self.ifile = open(ifile, "rb")
        self.ofile = open(ofile, "wb")

    def __del__(self):
        self.ifile.close()
        self.ofile.close()

    def deflate(self, blk_sz):
        ifile_sz = os.path.getsize(self.ifile_nm)
        count = (ifile_sz + int(blk_sz) - 1) // int(blk_sz)
        # Create array
        data = np.ndarray(count * 3, dtype=np.uint64)
        i = 0
        while i < count:
            # Each block of data is stored in temporary file that is used
            # by gzip.
            f = tempfile.NamedTemporaryFile(delete=False)
            blk = self.ifile.read(int(blk_sz))
            f.write(blk)
            f.close()
            of = tempfile.NamedTemporaryFile(delete=False)
            of.close()
            subprocess.run(["gzip", "-c", "--fast"], stdin=open(f.name, "rb"),
                           stdout=open(of.name, "wb"), check=True)
            if not i:
                subprocess.run(["cp", of.name, self.ofile_nm], check=True)
            else:
                with open(of.name, "rb") as src, open(self.ofile_nm, "ab") as dst:
                    dst.write(src.read())
            i += 1
            os.remove(f.name)
            os.remove(of.name)

    # Read block data from ifile. And inflate data.
    def inflate(self):
        ifile_sz = os.path.getsize(self.ifile_nm)
        while True:
            # by gunzip.
            f = tempfile.NamedTemporaryFile(delete=False)
            blk = self.ifile.read(ifile_sz)
            if not blk:
                break
            f.write(blk)
            f.close()
            subprocess.run(["gunzip"], stdin=open(f.name, "rb"),
                           stdout=open(self.ofile_nm, "wb"), check=True)
            os.remove(f.name)

def sw_deflate(ifile, ofile, blk_sz):
    dfl = listcontent(ifile, ofile)
    dfl.deflate(blk_sz)

def sw_inflate(ifile, ofile):
    ifl = listcontent(ifile, ofile)
    ifl.inflate()

def main(argv):
    ifile = ''
    ofile = ''
    blk_sz = 0
    try:
        opts, args = getopt.getopt(argv, "hb:", ["in=","out="])
        for opt, arg in opts:
            if opt in ("-h"):
                print('Software deflate command:')
                print('    list_loader -b <block size> --in <file> --out <file>')
                print('Software inflate command:')
                print('    list_loader --in <file> --out <file>')
                sys.exit()
            elif opt in ("-b"):
                blk_sz = arg
            elif opt in ("--in"):
                if not os.path.isfile(arg):
                    print("File does not exist:", arg)
                    sys.exit(1)
                ifile = arg
            elif opt in ("--out"):
                ofile = arg
    except getopt.GetoptError:
        # Compress source to destination file
        print('Software deflate command:')
        print('    list_loader -b <block size> --in <file> --out <file>')
        # Decompress source to destination file
        print('Software inflate command:')
        print('    list_loader --in <file> --out <file>')
        sys.exit(2)

    if blk_sz:
        sw_deflate(ifile, ofile, blk_sz)
    else:
        sw_inflate(ifile, ofile)

if __name__ == "__main__":
    main(sys.argv[1:])
