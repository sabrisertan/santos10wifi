#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only GPT inspection of a disk/image. Omits unique GUIDs and device IDs."""
import argparse,fcntl,json,os,stat,struct,zlib

def inspect(path,sector_size=512):
 fd=os.open(path,os.O_RDONLY)
 try:
  st=os.fstat(fd)
  if stat.S_ISBLK(st.st_mode):
   request=(2<<30)|(struct.calcsize('P')<<16)|(0x12<<8)|114  # BLKGETSIZE64 uses sizeof(size_t)
   size=struct.unpack('Q',fcntl.ioctl(fd,request,bytes(8)))[0]
  elif stat.S_ISREG(st.st_mode):size=st.st_size
  else:raise ValueError('Input must be a regular disk image or a block device')
  if size<3*sector_size or size%sector_size:raise ValueError('Invalid disk/sector size')
  def read(offset,length):
   if offset<0 or length<0 or offset+length>size:raise ValueError('GPT range outside disk')
   data=os.pread(fd,length,offset)
   if len(data)!=length:raise ValueError('Short disk read')
   return data
  def header(lba):
   raw=read(lba*sector_size,sector_size)
   if raw[:8]!=b'EFI PART':raise ValueError(f'No GPT signature at LBA {lba}')
   length,stored=struct.unpack_from('<II',raw,12)
   if not 92<=length<=sector_size:raise ValueError('Unsupported GPT header length')
   h=bytearray(raw[:length]);h[16:20]=bytes(4)
   current,other,first,last=struct.unpack_from('<QQQQ',raw,24)
   entries_lba,count,stride,crc=struct.unpack_from('<QIII',raw,72)
   if stride<128 or stride%8 or count*stride>16*1024*1024:raise ValueError('Invalid or excessive GPT array')
   entries=read(entries_lba*sector_size,count*stride);parts=[]
   for i in range(count):
    e=entries[i*stride:(i+1)*stride]
    if e[:16]==bytes(16):continue
    start,end=struct.unpack_from('<QQ',e,32)
    name=e[56:128].decode('utf-16-le',errors='replace').split('\0',1)[0]
    parts.append({'number':i+1,'name':name,'start_sector':start,'sectors':end-start+1,'within_usable_range':first<=start<=end<=last})
   return {'header_lba':lba,'header_size':length,'current_lba_matches':current==lba,'other_header_lba':other,
           'header_crc_valid':zlib.crc32(h)&0xffffffff==stored,'array_crc_valid':zlib.crc32(entries)&0xffffffff==crc,
           'array_crc32':f'{crc:08x}','entry_capacity':count,'used_entries':len(parts),'partitions':parts}
  primary,alternate=header(1),header(size//sector_size-1)
  return {'sector_size':sector_size,'disk_sectors':size//sector_size,'primary':primary,'alternate':alternate,
          'partition_arrays_differ':primary['array_crc32']!=alternate['array_crc32'],
          'note':'Read only. No repair or automatic table-selection recommendation.'}
 finally:os.close(fd)

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('disk');p.add_argument('--sector-size',type=int,choices=[512,4096],default=512);a=p.parse_args()
 try:print(json.dumps(inspect(a.disk,a.sector_size),indent=2))
 except (ValueError,OSError) as e:p.exit(1,f'{e}\n')
