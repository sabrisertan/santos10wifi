#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise GPT CRC/geometry parsing without a block device or root privileges."""
import importlib.util,struct,tempfile,unittest,zlib
from pathlib import Path
spec=importlib.util.spec_from_file_location('inspect_gpt',Path(__file__).resolve().parents[1]/'tools/inspect-gpt.py')
gpt=importlib.util.module_from_spec(spec);spec.loader.exec_module(gpt)

class GPTTests(unittest.TestCase):
 def fixture(self):
  disk=bytearray(128*512)
  for lba,other,table,header_size,start in [(1,127,2,512,10),(127,1,126,92,12)]:
   entries=bytearray(512);entries[0:16]=b'X'*16
   struct.pack_into('<QQ',entries,32,start,50)
   entries[56:56+len('ROOT'.encode('utf-16-le'))]='ROOT'.encode('utf-16-le')
   disk[table*512:(table+1)*512]=entries
   h=bytearray(512);h[:8]=b'EFI PART'
   struct.pack_into('<IIII',h,8,0x10000,header_size,0,0)
   struct.pack_into('<QQQQ',h,24,lba,other,3,125)
   struct.pack_into('<QIII',h,72,table,4,128,zlib.crc32(entries)&0xffffffff)
   struct.pack_into('<I',h,16,zlib.crc32(h[:header_size])&0xffffffff)
   disk[lba*512:(lba+1)*512]=h
  return disk
 def inspect(self,data):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td)/'disk.img';p.write_bytes(data);result=gpt.inspect(p)
   self.assertEqual(p.read_bytes(),data,'Inspection must not modify input')
   return result
 def test_samsung_and_standard_headers_with_different_valid_arrays(self):
  result=self.inspect(self.fixture())
  self.assertTrue(result['partition_arrays_differ'])
  for key in ('primary','alternate'):
   self.assertTrue(result[key]['header_crc_valid']);self.assertTrue(result[key]['array_crc_valid'])
   self.assertEqual(result[key]['partitions'][0]['name'],'ROOT')
 def test_corrupted_array_is_reported(self):
  disk=self.fixture();disk[2*512+100]^=1
  self.assertFalse(self.inspect(disk)['primary']['array_crc_valid'])
 def test_corrupted_header_is_reported(self):
  disk=self.fixture();disk[512+100]^=1
  self.assertFalse(self.inspect(disk)['primary']['header_crc_valid'])
 def test_excessive_array_is_rejected(self):
  disk=self.fixture();struct.pack_into('<I',disk,512+80,0xffffffff)
  with self.assertRaisesRegex(ValueError,'excessive'):self.inspect(disk)
 def test_array_outside_disk_is_rejected(self):
  disk=self.fixture();struct.pack_into('<Q',disk,512+72,128)
  with self.assertRaisesRegex(ValueError,'outside disk'):self.inspect(disk)

if __name__=='__main__':unittest.main()
