/*
  Copyright (C) 2021-2023  Selwin van Dijk

  This file is part of signalbackup-tools.

  signalbackup-tools is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  signalbackup-tools is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with signalbackup-tools.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "filedecryptor.ih"

#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
void FileDecryptor::strugee(uint64_t pos)
{
  uint offset = 0;

  d_file.seekg(pos, std::ios_base::beg);

  std::cout << "Getting frame at filepos: " << d_file.tellg() << std::endl;

  if (static_cast<uint64_t>(d_file.tellg()) == d_filesize)
  {
    std::cout << "Read entire backup file..." << std::endl;
    return;
  }

  uint32_t encryptedframelength = getNextFrameBlockSize();
  if (encryptedframelength > 3145728/*= 3MB*/ /*115343360 / * =110MB*/ || encryptedframelength < 11)
  {
    std::cout << "Framesize too big to be real" << std::endl;
    return;
  }

  std::unique_ptr<unsigned char[]> encryptedframe(new unsigned char[encryptedframelength]);
  if (!getNextFrameBlock(encryptedframe.get(), encryptedframelength))
    return;

  // check hash
  unsigned int digest_size = SHA256_DIGEST_LENGTH;
  unsigned char hash[SHA256_DIGEST_LENGTH];
  HMAC(EVP_sha256(), d_mackey, d_mackey_size, encryptedframe.get(), encryptedframelength - MACSIZE, hash, &digest_size);
  if (std::memcmp(encryptedframe.get() + (encryptedframelength - MACSIZE), hash, MACSIZE) != 0)
  {
    std::cout << "BAD MAC!" << std::endl;
    return;
  }
  else
  {
    std::cout << "" << std::endl;
    std::cout << "GOT GOOD MAC AT OFFSET " << offset << " BYTES!" << std::endl;
    std::cout << "Now let's try and find out how many frames we skipped to get here...." << std::endl;
    d_badmac = false;
  }

  // decode
  uint skipped = 0;
  std::unique_ptr<BackupFrame> frame(nullptr);
  while (!frame)
  {

    if (skipped > 1000000) // a frame is at least 10 bytes? -> could probably safely set this higher. MAC alone is 10 bytes, there is also actual data
    {
      std::cout << "TESTED 1000000 frames" << std::endl;
      return;
    }

    if (skipped % 100 == 0)
      std::cout << "\rChecking if we skipped " << skipped << " frames... " << std::flush;

    uintToFourBytes(d_iv, d_counter + skipped);

    // create context
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), &::EVP_CIPHER_CTX_free);

    // disable padding
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_ctr(), nullptr, d_cipherkey, d_iv) != 1)
    {
      std::cout << "CTX INIT FAILED" << std::endl;
      return;
    }

    int decodedframelength = encryptedframelength - MACSIZE;
    unsigned char *decodedframe = new unsigned char[decodedframelength];

    if (EVP_DecryptUpdate(ctx.get(), decodedframe, &decodedframelength, encryptedframe.get(), encryptedframelength - MACSIZE) != 1)
    {
      std::cout << "Failed to decrypt data" << std::endl;
      delete[] decodedframe;
      return;
    }

    DEBUGOUT("Decoded hex      : ", bepaald::bytesToHexString(decodedframe, decodedframelength));

    frame.reset(initBackupFrame(decodedframe, decodedframelength, d_framecount + skipped));

    delete[] decodedframe;

    ++skipped;
    if (!frame)
    {
      std::cout << "\rChecking if we skipped " << skipped << " frames... nope! :(" << std::flush;
      //if (skipped >
    }
    else
    {
      if (frame->validate() &&
          frame->frameType() != BackupFrame::FRAMETYPE::HEADER && // it is impossible to get in this function without the headerframe, and there is only one
          (frame->frameType() != BackupFrame::FRAMETYPE::END || static_cast<uint64_t>(d_file.tellg()) == d_filesize))
      {
        d_counter += skipped;
        d_framecount += skipped;
        std::cout << "\rChecking if we skipped " << skipped << " frames... YEAH! :)" << std::endl;
        std::cout << "Good frame: " << frame->frameNumber() << " (" << frame->frameTypeString() << ")" << std::endl;
        std::cout << "COUNTER: " << d_counter << std::endl;
        frame->printInfo();
        //delete[] encryptedframe.release();
        frame.reset();
        return;
      }
      std::cout << "\rChecking if we skipped " << skipped << " frames... nope! :(" << std::flush;
      frame.reset();
    }
  }

  //frame->printInfo();
  //std::cout << "HEADERTYPE: " << frame->frameType() << std::endl;

  uint32_t attsize = 0;
  if (!d_badmac && (attsize = frame->attachmentSize()) > 0 &&
      (frame->frameType() == BackupFrame::FRAMETYPE::ATTACHMENT ||
       frame->frameType() == BackupFrame::FRAMETYPE::AVATAR ||
       frame->frameType() == BackupFrame::FRAMETYPE::STICKER))
  {
    if (d_verbose) [[unlikely]]
      std::cout << "Trying to read attachment (bruteforce)" << std::endl;

    uintToFourBytes(d_iv, d_counter++);

    reinterpret_cast<FrameWithAttachment *>(frame.get())->setLazyData(d_iv, d_iv_size, d_mackey, d_mackey_size, d_cipherkey, d_cipherkey_size, attsize, d_filename, d_file.tellg());

    d_file.seekg(attsize + MACSIZE, std::ios_base::cur);

    /*
    if (!d_lazyload) // immediately decrypt i guess...
    {
      if (d_verbose) [[unlikely]]
        std::cout << "Getting attachment at file pos " << d_file.tellg() << " (size: " << attsize << ")" << std::endl;

      int getatt = getAttachment(reinterpret_cast<FrameWithAttachment *>(frame.get()));
      if (getatt != 0)
      {
        if (getatt < 0)
          d_badmac = true;
        return;
      }
    }
    */
  }

}



#include "../sqlstatementframe/sqlstatementframe.h"

std::unique_ptr<BackupFrame> FileDecryptor::getFrameStrugee2()
{
  long long int filepos = d_file.tellg();

  if (d_verbose) [[unlikely]]
    std::cout << "Getting frame at filepos: " << filepos << " (COUNTER: " << d_counter << ")" << std::endl;

  if (static_cast<uint64_t>(filepos) == d_filesize) [[unlikely]]
  {
    std::cout << "Read entire backup file..." << std::endl;
    return std::unique_ptr<BackupFrame>(nullptr);
  }

  if (d_headerframe)
  {
    std::unique_ptr<BackupFrame> frame(d_headerframe.release());
    return frame;
  }

  uint32_t encryptedframelength = getNextFrameBlockSize();
  //if (encryptedframelength > 3145728/*= 3MB*/ /*115343360 / * =110MB*/ || encryptedframelength < 11)
  //{
  //  std::cout << "Suspicious framelength" << std::endl;
  //  bruteForceFrom(filepos)???
  //}

  if (encryptedframelength == 0 && d_file.eof()) [[unlikely]]
  {
    std::cout << bepaald::bold_on << "ERROR" << bepaald::bold_off << " Unexpectedly hit end of file!" << std::endl;
    return std::unique_ptr<BackupFrame>(nullptr);
  }

  DEBUGOUT("Framelength: ", encryptedframelength);
  if (d_verbose) [[unlikely]]
    std::cout << "Framelength: " << encryptedframelength << std::endl;

  std::unique_ptr<unsigned char[]> encryptedframe(new unsigned char[encryptedframelength]);
  if (encryptedframelength > 115343360 /*110MB*/ || encryptedframelength < 11 || !getNextFrameBlock(encryptedframe.get(), encryptedframelength)) [[unlikely]]
  {
    std::cout << "Failed to read next frame (" << encryptedframelength << " bytes at filepos " << filepos << ")" << std::endl;
    return std::unique_ptr<BackupFrame>(nullptr);
  }

  // check hash
  unsigned int digest_size = SHA256_DIGEST_LENGTH;
  unsigned char hash[SHA256_DIGEST_LENGTH];
  HMAC(EVP_sha256(), d_mackey, d_mackey_size, encryptedframe.get(), encryptedframelength - MACSIZE, hash, &digest_size);
  if (std::memcmp(encryptedframe.get() + (encryptedframelength - MACSIZE), hash, 10) != 0) [[unlikely]]
  {
    std::cout << "" << std::endl;
    std::cout << "WARNING: Bad MAC in frame: theirMac: " << bepaald::bytesToHexString(encryptedframe.get() + (encryptedframelength - MACSIZE), MACSIZE) << std::endl;
    std::cout << "                             ourMac: " << bepaald::bytesToHexString(hash, SHA256_DIGEST_LENGTH) << std::endl;
    d_badmac = true;
    if (d_stoponerror)
    {
      std::cout << "Stop reading backup. Next frame would be read at offset " << filepos + encryptedframelength << std::endl;
      return std::unique_ptr<BackupFrame>(nullptr);
    }
  }
  else
  {
    d_badmac = false;
    if (d_verbose) [[unlikely]]
    {
      std::cout << "Calculated mac: " << bepaald::bytesToHexString(hash, SHA256_DIGEST_LENGTH) << std::endl;
      std::cout << "Mac in file   : " << bepaald::bytesToHexString(encryptedframe.get() + (encryptedframelength - MACSIZE), MACSIZE) << std::endl;
    }
  }

  // decode
  uintToFourBytes(d_iv, d_counter++);

  // create context
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), &::EVP_CIPHER_CTX_free);

  // disable padding
  EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_ctr(), nullptr, d_cipherkey, d_iv) != 1) [[unlikely]]
  {
    std::cout << "CTX INIT FAILED" << std::endl;
    return std::unique_ptr<BackupFrame>(nullptr);
  }

  int decodedframelength = encryptedframelength - MACSIZE;
  unsigned char *decodedframe = new unsigned char[decodedframelength];

  if (EVP_DecryptUpdate(ctx.get(), decodedframe, &decodedframelength, encryptedframe.get(), encryptedframelength - MACSIZE) != 1) [[unlikely]]
  {
    std::cout << "Failed to decrypt data" << std::endl;
    delete[] decodedframe;
    return std::unique_ptr<BackupFrame>(nullptr);
  }

  delete[] encryptedframe.release(); // free up already....

  std::unique_ptr<BackupFrame> frame(initBackupFrame(decodedframe, decodedframelength, d_framecount++));

  if (!frame) [[unlikely]]
  {
    std::cout << "Failed to get valid frame from decoded data..." << std::endl;
    if (d_badmac)
    {
      std::cout << "Encrypted data had failed verification (Bad MAC)" << std::endl;
      delete[] decodedframe;
      return std::unique_ptr<BackupFrame>(nullptr);
    }
    else
    {
      std::cout << "Data was verified ok, but does not represent a valid frame... Don't know what happened, but it's bad... :(" << std::endl;
      std::cout << "Decrypted frame data: " << bepaald::bytesToHexString(decodedframe, decodedframelength) << std::endl;
      delete[] decodedframe;
      return std::make_unique<InvalidFrame>();
    }
    delete[] decodedframe;
    return std::unique_ptr<BackupFrame>(nullptr);
  }

  delete[] decodedframe;

  uint32_t attsize = frame->attachmentSize();
  if (!d_badmac && attsize > 0 &&
      (frame->frameType() == BackupFrame::FRAMETYPE::ATTACHMENT ||
       frame->frameType() == BackupFrame::FRAMETYPE::AVATAR ||
       frame->frameType() == BackupFrame::FRAMETYPE::STICKER))
  {

    if ((d_file.tellg() < 0 && d_file.eof()) || (attsize + static_cast<uint64_t>(d_file.tellg()) > d_filesize)) [[unlikely]]
      if (!d_assumebadframesize)
      {
        std::cout << bepaald::bold_on << "ERROR" << bepaald::bold_off << " Unexpectedly hit end of file while reading attachment!" << std::endl;
        return std::unique_ptr<BackupFrame>(nullptr);
      }

    uintToFourBytes(d_iv, d_counter++);

    reinterpret_cast<FrameWithAttachment *>(frame.get())->setLazyData(d_iv, d_iv_size, d_mackey, d_mackey_size, d_cipherkey, d_cipherkey_size, attsize, d_filename, d_file.tellg());

    d_file.seekg(attsize + MACSIZE, std::ios_base::cur);

    /*
    if (!d_lazyload) // immediately decrypt i guess...
    {
      if (d_verbose) [[unlikely]]
        std::cout << "Getting attachment at file pos " << d_file.tellg() << " (size: " << attsize << ")" << std::endl;

      int getatt = getAttachment(reinterpret_cast<FrameWithAttachment *>(frame.get())); // 0 == good, >0 == bad, <0 == bad+badmac
      if (getatt > 0)
      {
        std::cout << "Failed to get attachment data for FrameWithAttachment... info:" << std::endl;
        frame->printInfo();
        return std::unique_ptr<BackupFrame>(nullptr);
      }
      if (getatt < 0)
      {
        d_badmac = true;
        if (d_stoponerror)
        {
          std::cout << "Stop reading backup. Next frame would be read at offset " << filepos + encryptedframelength << std::endl;
          return std::unique_ptr<BackupFrame>(nullptr);
        }
        if (d_assumebadframesize)
        {
          std::unique_ptr<BackupFrame> f = bruteForceFrom(filepos, encryptedframelength);
          //long long int curfilepos = d_file.tellg();
          //std::cout << "curpso: " << curfilepos << std::endl;
          //std::cout << "ATTACHMENT LENGTH SHOULD HAVE BEEN: " << curfilepos - filepos - encryptedframelength - MACSIZE << std::endl;
          return f;
        }
      }
    }
    */

  }

  //std::cout << "FILEPOS: " << d_file.tellg() << std::endl;

  //delete frame;

  return frame;
}

void FileDecryptor::strugee2()
{

  d_stoponerror = true;

  std::vector<std::string> tables;
  std::string lastmsg;
  bool endfound = false;

  std::unique_ptr<BackupFrame> frame(nullptr);
  while ((frame = getFrameStrugee2()))
  {
    if (frame->frameType() == BackupFrame::FRAMETYPE::SQLSTATEMENT)
    {
      SqlStatementFrame *s = reinterpret_cast<SqlStatementFrame *>(frame.get());
      if (s->statement().find("INSERT INTO ") == 0)
      {
        // parse table name
        std::string::size_type pos = s->statement().find(' ', 12);
        std::string tablename = s->statement().substr(12, pos - 12);

        if (std::find(tables.begin(), tables.end(), tablename) == tables.end())
          tables.push_back(tablename);

        if (tablename == "mms" || tablename == "message" || tablename == "sms")
          lastmsg = s->statement();
      }
    }
    if (frame->frameType() == BackupFrame::FRAMETYPE::END)
      endfound = true;
  }

  std::cout << "Tables present in backup:" << std::endl;
  for (uint i = 0; i < tables.size(); ++i)
    std::cout << tables[i] << ((i == tables.size() - 1) && !endfound ? " (probably incomplete)" : "") << std::endl;

  std::cout << "Last message: " << (lastmsg.empty() ? "(none)" : lastmsg) << std::endl;

}

void FileDecryptor::strugee3Helper(std::vector<std::pair<std::unique_ptr<unsigned char[]>, uint64_t>> *macs_and_positions)
{

  while (true)
  {
  //d_file.seekg(0, std::ios_base::beg);
  long long int filepos = d_file.tellg();

  //std::cout << "FILEPOS: " << filepos << std::endl;


  if (d_verbose) [[unlikely]]
    std::cout << "Getting frame at filepos: " << filepos << " (COUNTER: " << d_counter << ")" << std::endl;

  if (static_cast<uint64_t>(filepos) == d_filesize) [[unlikely]]
  {
    std::cout << "Read entire backup file..." << std::endl;
    return;
  }

  if (d_headerframe)
  {
    std::unique_ptr<BackupFrame> frame(d_headerframe.release());
    std::cout << "Headerframe" << std::endl;
    continue;
  }

  uint32_t encryptedframelength = getNextFrameBlockSize();
  //if (encryptedframelength > 3145728/*= 3MB*/ /*115343360 / * =110MB*/ || encryptedframelength < 11)
  //{
  //  std::cout << "Suspicious framelength" << std::endl;
  //  bruteForceFrom(filepos)???
  //}

  if (encryptedframelength == 0 && d_file.eof()) [[unlikely]]
  {
    std::cout << bepaald::bold_on << "ERROR" << bepaald::bold_off << " Unexpectedly hit end of file!" << std::endl;
    return;
  }

  DEBUGOUT("Framelength: ", encryptedframelength);
  if (d_verbose) [[unlikely]]
    std::cout << "Framelength: " << encryptedframelength << std::endl;

  std::unique_ptr<unsigned char[]> encryptedframe(new unsigned char[encryptedframelength]);
  if (encryptedframelength > 115343360 /*110MB*/ || encryptedframelength < 11 || !getNextFrameBlock(encryptedframe.get(), encryptedframelength)) [[unlikely]]
  {
    std::cout << "Failed to read next frame (" << encryptedframelength << " bytes at filepos " << filepos << ")" << std::endl;
    return;
  }

  // check hash
  unsigned int digest_size = SHA256_DIGEST_LENGTH;
  unsigned char hash[SHA256_DIGEST_LENGTH];
  HMAC(EVP_sha256(), d_mackey, d_mackey_size, encryptedframe.get(), encryptedframelength - MACSIZE, hash, &digest_size);
  if (std::memcmp(encryptedframe.get() + (encryptedframelength - MACSIZE), hash, 10) != 0) [[unlikely]]
  {
    std::cout << "" << std::endl;
    std::cout << "WARNING: Bad MAC in frame: theirMac: " << bepaald::bytesToHexString(encryptedframe.get() + (encryptedframelength - MACSIZE), MACSIZE) << std::endl;
    std::cout << "                             ourMac: " << bepaald::bytesToHexString(hash, SHA256_DIGEST_LENGTH) << std::endl;
    d_badmac = true;
  }
  else
  {

    macs_and_positions->emplace_back(std::make_pair(new unsigned char[SHA256_DIGEST_LENGTH], filepos));
    std::memcpy(macs_and_positions->back().first.get(), hash, SHA256_DIGEST_LENGTH);

    d_badmac = false;
    if (d_verbose) [[unlikely]]
    {
      std::cout << "Calculated mac: " << bepaald::bytesToHexString(hash, SHA256_DIGEST_LENGTH) << std::endl;
      std::cout << "Mac in file   : " << bepaald::bytesToHexString(encryptedframe.get() + (encryptedframelength - MACSIZE), MACSIZE) << std::endl;
    }
  }


  // decode
  uintToFourBytes(d_iv, d_counter++);

  // create context
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), &::EVP_CIPHER_CTX_free);

  // disable padding
  EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_ctr(), nullptr, d_cipherkey, d_iv) != 1) [[unlikely]]
  {
    std::cout << "CTX INIT FAILED" << std::endl;
    return;
  }

  int decodedframelength = encryptedframelength - MACSIZE;
  unsigned char *decodedframe = new unsigned char[decodedframelength];

  if (EVP_DecryptUpdate(ctx.get(), decodedframe, &decodedframelength, encryptedframe.get(), encryptedframelength - MACSIZE) != 1) [[unlikely]]
  {
    std::cout << "Failed to decrypt data" << std::endl;
    delete[] decodedframe;
    return;
  }

  delete[] encryptedframe.release(); // free up already....

  std::unique_ptr<BackupFrame> frame(initBackupFrame(decodedframe, decodedframelength, d_framecount++));

  if (!frame) [[unlikely]]
  {
    std::cout << "Failed to get valid frame from decoded data..." << std::endl;
    if (d_badmac)
    {
      std::cout << "Encrypted data had failed verification (Bad MAC)" << std::endl;
      delete[] decodedframe;
      return;
    }
    else
    {
      std::cout << "Data was verified ok, but does not represent a valid frame... Don't know what happened, but it's bad... :(" << std::endl;
      std::cout << "Decrypted frame data: " << bepaald::bytesToHexString(decodedframe, decodedframelength) << std::endl;
      delete[] decodedframe;
      return;
    }
    delete[] decodedframe;
    return;
  }

  delete[] decodedframe;

  uint32_t attsize = frame->attachmentSize();
  if (!d_badmac && attsize > 0 &&
      (frame->frameType() == BackupFrame::FRAMETYPE::ATTACHMENT ||
       frame->frameType() == BackupFrame::FRAMETYPE::AVATAR ||
       frame->frameType() == BackupFrame::FRAMETYPE::STICKER))
  {

    if ((d_file.tellg() < 0 && d_file.eof()) || (attsize + static_cast<uint64_t>(d_file.tellg()) > d_filesize)) [[ unlikely ]]
      if (!d_assumebadframesize)
      {
        std::cout << bepaald::bold_on << "ERROR" << bepaald::bold_off << " Unexpectedly hit end of file while reading attachment!" << std::endl;
        return;
      }

    uintToFourBytes(d_iv, d_counter++);

    reinterpret_cast<FrameWithAttachment *>(frame.get())->setLazyData(d_iv, d_iv_size, d_mackey, d_mackey_size, d_cipherkey, d_cipherkey_size, attsize, d_filename, d_file.tellg());

    d_file.seekg(attsize + MACSIZE, std::ios_base::cur);

    /*
    if (!d_lazyload) // immediately decrypt i guess...
    {
      if (d_verbose) [[unlikely]]
        std::cout << "Getting attachment at file pos " << d_file.tellg() << " (size: " << attsize << ")" << std::endl;

      int getatt = getAttachment(reinterpret_cast<FrameWithAttachment *>(frame.get())); // 0 == good, >0 == bad, <0 == bad+badmac
      if (getatt > 0)
      {
        std::cout << "Failed to get attachment data for FrameWithAttachment... info:" << std::endl;
        frame->printInfo();
        return;
      }
      if (getatt < 0)
      {
        d_badmac = true;
        if (d_stoponerror)
        {
          std::cout << "Stop reading backup. Next frame would be read at offset " << filepos + encryptedframelength << std::endl;
          return;
        }
        if (d_assumebadframesize)
        {
          std::unique_ptr<BackupFrame> f = bruteForceFrom(filepos, encryptedframelength);
          //long long int curfilepos = d_file.tellg();
          //std::cout << "curpso: " << curfilepos << std::endl;
          //std::cout << "ATTACHMENT LENGTH SHOULD HAVE BEEN: " << curfilepos - filepos - encryptedframelength - MACSIZE << std::endl;
          return;
        }
      }
    }
    */
  }







  }
}

void FileDecryptor::strugee3(uint64_t pos)
{

  std::vector<std::pair<std::unique_ptr<unsigned char[]>, uint64_t>> macs_and_positions;
  strugee3Helper(&macs_and_positions);
  std::cout << "Got macs: " << std::endl;
  //for (uint i = 0; i < macs_and_positions.size(); ++i)
  //  std::cout << macs_and_positions[i].second << " : " << bepaald::bytesToHexString(macs_and_positions[i].first.get(), SHA256_DIGEST_LENGTH) << std::endl;

  uint offset = 0;

  d_file.seekg(pos, std::ios_base::beg);

  std::cout << "Getting frame at filepos: " << d_file.tellg() << std::endl;

  if (static_cast<uint64_t>(d_file.tellg()) == d_filesize)
  {
    std::cout << "Read entire backup file..." << std::endl;
    return;
  }

  uint32_t encryptedframelength = getNextFrameBlockSize();
  if (encryptedframelength > 3145728/*= 3MB*/ /*115343360 / * =110MB*/ || encryptedframelength < 11)
  {
    std::cout << "Framesize too big to be real" << std::endl;
    return;
  }

  std::unique_ptr<unsigned char[]> encryptedframe(new unsigned char[encryptedframelength]);
  if (!getNextFrameBlock(encryptedframe.get(), encryptedframelength))
    return;

  // check hash
  unsigned int digest_size = SHA256_DIGEST_LENGTH;
  unsigned char hash[SHA256_DIGEST_LENGTH];
  HMAC(EVP_sha256(), d_mackey, d_mackey_size, encryptedframe.get(), encryptedframelength - MACSIZE, hash, &digest_size);
  if (std::memcmp(encryptedframe.get() + (encryptedframelength - MACSIZE), hash, MACSIZE) != 0)
  {
    std::cout << "BAD MAC!" << std::endl;
    return;
  }
  else
  {
    std::cout << "" << std::endl;
    std::cout << "GOT GOOD MAC AT OFFSET " << offset << " BYTES!" << std::endl;
    std::cout << "Now let's try and find out how many frames we skipped to get here...." << std::endl;
    d_badmac = false;
  }




  std::cout << "Got GOOD MAC : " << bepaald::bytesToHexString(hash, SHA256_DIGEST_LENGTH) << std::endl;
  for (uint i = 0; i < macs_and_positions.size(); ++i)
  {
    if (std::memcmp(macs_and_positions[i].first.get(), hash, SHA256_DIGEST_LENGTH) == 0)
    {
      std::cout << "SAME MAC AT POS: " << macs_and_positions[i].second << std::endl;

      int const size = 200;
      unsigned char bytes[size];

      d_file.seekg(macs_and_positions[i].second);
      d_file.read(reinterpret_cast<char *>(bytes), size);
      std::cout << "200 bytes at file position " << macs_and_positions[i].second << ": " << std::endl << bepaald::bytesToHexString(bytes, size) << std::endl;

      d_file.seekg(pos);
      d_file.read(reinterpret_cast<char *>(bytes), size);
      std::cout << "200 bytes at file position " << pos << ": " << std::endl << bepaald::bytesToHexString(bytes, size) << std::endl;
    }
  }
}

void FileDecryptor::ashmorgan()
{
  uint offset = 0;


  d_file.seekg(d_filesize - 100, std::ios_base::beg);
  std::unique_ptr<unsigned char[]> cbytes(new unsigned char[100]);
  d_file.read(reinterpret_cast<char *>(cbytes.get()), 100);
  std::cout << std::endl << "Last 100 bytes: " << bepaald::bytesToHexString(cbytes.get(), 100) << std::endl << std::endl;
  cbytes.release();



  d_file.seekg(d_filesize - 16, std::ios_base::beg);

  std::cout << "Getting frame at filepos: " << d_file.tellg() << std::endl;

  if (static_cast<uint64_t>(d_file.tellg()) == d_filesize)
  {
    std::cout << "Read entire backup file..." << std::endl;
    return;
  }

  uint32_t encryptedframelength = getNextFrameBlockSize();
  if (encryptedframelength > 3145728/*= 3MB*/ /*115343360 / * =110MB*/ || encryptedframelength < 11)
  {
    std::cout << "Framesize too big to be real" << std::endl;
    return;
  }

  std::unique_ptr<unsigned char[]> encryptedframe(new unsigned char[encryptedframelength]);
  if (!getNextFrameBlock(encryptedframe.get(), encryptedframelength))
    return;

  std::cout << "FRAME: " << bepaald::bytesToHexString(encryptedframe.get(), encryptedframelength) << std::endl;

  // check hash
  unsigned int digest_size = SHA256_DIGEST_LENGTH;
  unsigned char hash[SHA256_DIGEST_LENGTH];
  HMAC(EVP_sha256(), d_mackey, d_mackey_size, encryptedframe.get(), encryptedframelength - MACSIZE, hash, &digest_size);
  if (std::memcmp(encryptedframe.get() + (encryptedframelength - MACSIZE), hash, MACSIZE) != 0)
  {
    std::cout << "BAD MAC!" << std::endl;
    return;
  }
  else
  {
    std::cout << "" << std::endl;
    std::cout << "GOT GOOD MAC AT OFFSET " << offset << " BYTES! " << bepaald::bytesToHexString(hash, digest_size) << std::endl;
    std::cout << "Now let's try and find out how many frames we skipped to get here...." << std::endl;
    d_badmac = false;
  }

  // decode
  uint skipped = 0;
  std::unique_ptr<BackupFrame> frame(nullptr);
  while (true)
  {
    if (frame)
      frame.reset();

    if (skipped > 1000000) // a frame is at least 10 bytes? -> could probably safely set this higher. MAC alone is 10 bytes, there is also actual data
    {
      std::cout << "TESTED 1000000 frames" << std::endl;
      return;
    }

    if (skipped % 100 == 0)
      std::cout << "\rChecking if we skipped " << skipped << " frames... " << std::flush;

    uintToFourBytes(d_iv, d_counter + skipped);

    // create context
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), &::EVP_CIPHER_CTX_free);

    // disable padding
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_ctr(), nullptr, d_cipherkey, d_iv) != 1)
    {
      std::cout << "CTX INIT FAILED" << std::endl;
      return;
    }

    int decodedframelength = encryptedframelength - MACSIZE;
    unsigned char *decodedframe = new unsigned char[decodedframelength];

    if (EVP_DecryptUpdate(ctx.get(), decodedframe, &decodedframelength, encryptedframe.get(), encryptedframelength - MACSIZE) != 1)
    {
      std::cout << "Failed to decrypt data" << std::endl;
      delete[] decodedframe;
      return;
    }

    DEBUGOUT("Decoded hex      : ", bepaald::bytesToHexString(decodedframe, decodedframelength));

    frame.reset(initBackupFrame(decodedframe, decodedframelength, d_framecount + skipped));

    ++skipped;
    if (!frame)
    {
      std::cout << "\rChecking if we skipped " << skipped << " frames... nope! :(" << std::flush;
      //if (skipped >
    }
    else
    {
      if (frame->validate() &&
          frame->frameType() != BackupFrame::FRAMETYPE::HEADER && // it is impossible to get in this function without the headerframe, and there is only one
          (frame->frameType() != BackupFrame::FRAMETYPE::END || static_cast<uint64_t>(d_file.tellg()) == d_filesize))
      {
        d_counter += skipped;
        d_framecount += skipped;
        std::cout << "\rChecking if we skipped " << skipped << " frames... YEAH! :)" << std::endl;
        std::cout << "Good frame: " << frame->frameNumber() << " (" << frame->frameTypeString() << ")" << std::endl;
        std::cout << "COUNTER: " << d_counter << std::endl;
        std::cout << "Decoded hex      : " << bepaald::bytesToHexString(decodedframe, decodedframelength) << std::endl;
        frame->printInfo();
        //delete[] encryptedframe.release();
        frame.reset();

        delete[] decodedframe;
        return;
      }
      std::cout << "\rChecking if we skipped " << skipped << " frames... nope! :(" << std::flush;
      frame.reset();
    }
    delete[] decodedframe;
  }

  //frame->printInfo();
  //std::cout << "HEADERTYPE: " << frame->frameType() << std::endl;

  uint32_t attsize = 0;
  if (!d_badmac && (attsize = frame->attachmentSize()) > 0 &&
      (frame->frameType() == BackupFrame::FRAMETYPE::ATTACHMENT ||
       frame->frameType() == BackupFrame::FRAMETYPE::AVATAR ||
       frame->frameType() == BackupFrame::FRAMETYPE::STICKER))
  {
    if (d_verbose) [[unlikely]]
      std::cout << "Trying to read attachment (bruteforce)" << std::endl;

    uintToFourBytes(d_iv, d_counter++);

    reinterpret_cast<FrameWithAttachment *>(frame.get())->setLazyData(d_iv, d_iv_size, d_mackey, d_mackey_size, d_cipherkey, d_cipherkey_size, attsize, d_filename, d_file.tellg());

    d_file.seekg(attsize + MACSIZE, std::ios_base::cur);

    /*
    if (!d_lazyload) // immediately decrypt i guess...
    {
      if (d_verbose) [[unlikely]]
        std::cout << "Getting attachment at file pos " << d_file.tellg() << " (size: " << attsize << ")" << std::endl;

      int getatt = getAttachment(reinterpret_cast<FrameWithAttachment *>(frame.get()));
      if (getatt != 0)
      {
        if (getatt < 0)
          d_badmac = true;
        return;
      }
    }
    */
  }

}
