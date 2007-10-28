/*
 * Copyright (C) 2006 Tommi Maekitalo
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * is provided AS IS, WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, and
 * NON-INFRINGEMENT.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#include <zeno/fileimpl.h>
#include <zeno/error.h>
#include <zeno/article.h>
#include <zeno/dirent.h>
#include <zeno/qunicode.h>
#include <cxxtools/log.h>
#include <tnt/deflatestream.h>
#include <sstream>

log_define("zeno.file.impl")

namespace zeno
{
  //////////////////////////////////////////////////////////////////////
  // FileImpl
  //
  FileImpl::FileImpl(const char* fname)
    : zenoFile(fname)
  {
    if (!zenoFile)
      throw ZenoFileFormatError(std::string("can't open zeno-file \"") + fname + '"');

    filename = fname;

    const unsigned headerSize = 0x3c;
    char header[headerSize];
    if (!zenoFile.read(header, headerSize) || zenoFile.gcount() !=  headerSize)
      throw ZenoFileFormatError("format-error: header too short in zeno-file");

    size_type rMagic = fromLittleEndian<size_type>(header + 0x0);
    if (rMagic != 1439867043)
    {
      std::ostringstream msg;
      msg << "invalid magic number " << rMagic << " found - 1439867043 expected";
      throw ZenoFileFormatError(msg.str());
    }

    size_type rVersion = fromLittleEndian<size_type>(header + 0x4);
    if (rVersion != 3)
    {
      std::ostringstream msg;
      msg << "invalid zenofile version " << rVersion << " found - 3 expected";
      throw ZenoFileFormatError(msg.str());
    }

    size_type rCount = fromLittleEndian<size_type>(header + 0x8);
    offset_type rIndexPos = fromLittleEndian<offset_type>(header + 0x10);
    size_type rIndexLen = fromLittleEndian<size_type>(header + 0x18);
    offset_type rIndexPtrPos = fromLittleEndian<offset_type>(header + 0x20);
    size_type rIndexPtrLen = fromLittleEndian<size_type>(header + 0x28);

    log_debug("read " << rIndexPtrLen << " bytes");
    std::vector<size_type> buffer(rCount);
    zenoFile.seekg(rIndexPtrPos);
    zenoFile.read(reinterpret_cast<char*>(&buffer[0]), rIndexPtrLen);

    indexOffsets.reserve(rCount);
    for (std::vector<size_type>::const_iterator it = buffer.begin();
         it != buffer.end(); ++it)
      indexOffsets.push_back(static_cast<offset_type>(rIndexPos + fromLittleEndian<size_type>(&*it)));

    log_debug("read " << indexOffsets.size() << " index-entries ready");
  }

  Article FileImpl::getArticle(char ns, const QUnicodeString& url, bool collate)
  {
    log_debug("get article " << ns << " \"" << url << '"');
    std::pair<bool, size_type> s = findArticle(ns, url, collate);
    if (!s.first)
    {
      log_warn("article \"" << url << "\" not found");
      return Article();
    }

    Dirent d = readDirentNolock(indexOffsets[s.second]);

    log_info("article \"" << url << "\" size " << d.getSize() << " mime-type " << d.getMimeType());

    return Article(s.second, d, File(this));
  }

  Article FileImpl::getArticle(char ns, const std::string& url, bool collate)
  {
    return getArticle(ns, QUnicodeString(url), collate);
  }

  std::pair<bool, size_type> FileImpl::findArticle(char ns, const QUnicodeString& title, bool collate)
  {
    log_debug("find article " << ns << " \"" << title << "\", " << collate);

    if (getNamespaces().find(ns) == std::string::npos)
    {
      log_debug("namespace " << ns << " not found");
      return std::pair<bool, size_type>(false, 0);
    }

    cxxtools::MutexLock lock(mutex);

    IndexOffsetsType::size_type l = 0;
    IndexOffsetsType::size_type u = getCountArticles();

    unsigned itcount = 0;
    while (u - l > 1)
    {
      ++itcount;
      IndexOffsetsType::size_type p = l + (u - l) / 2;
      Dirent d = readDirentNolock(indexOffsets[p]);

      int c = ns < d.getNamespace() ? -1
            : ns > d.getNamespace() ? 1
            : (collate ? title.compareCollate(QUnicodeString(d.getTitle()))
                       : title.compare(QUnicodeString(d.getTitle())));
      if (c < 0)
        u = p;
      else if (c > 0)
        l = p;
      else
      {
        log_debug("article found after " << itcount << " iterations");
        return std::pair<bool, size_type>(true, p);
      }
    }

    Dirent d = readDirentNolock(indexOffsets[l]);
    int c = collate ? title.compareCollate(QUnicodeString(d.getTitle()))
                    : title.compare(QUnicodeString(d.getTitle()));
    if (c == 0)
    {
      log_debug("article found after " << itcount << " iterations");
      return std::pair<bool, size_type>(true, l);
    }

    log_debug("article not found (\"" << d.getTitle() << "\" does not match");
    return std::pair<bool, size_type>(false, u);
  }

  Article FileImpl::getArticle(size_type idx)
  {
    log_debug("getArticle(" << idx << ')');

    if (idx >= getCountArticles())
      throw ZenoFileFormatError("article index out of range");

    cxxtools::MutexLock lock(mutex);
    Dirent d = readDirentNolock(indexOffsets[idx]);
    return Article(idx, d, File(this));
  }

  Dirent FileImpl::getDirent(size_type idx)
  {
    if (idx >= getCountArticles())
      throw ZenoFileFormatError("article index out of range");

    cxxtools::MutexLock lock(mutex);
    return readDirentNolock(indexOffsets[idx]);
  }

  size_type FileImpl::getNamespaceBeginOffset(char ch)
  {
    size_type lower = 0;
    size_type upper = getCountArticles();
    Dirent d = getDirent(0);
    while (upper - lower > 1)
    {
      size_type m = lower + (upper - lower) / 2;
      Dirent d = getDirent(m);
      if (d.getNamespace() >= ch)
        upper = m;
      else
        lower = m;
    }
    return d.getNamespace() < ch ? upper : lower;
  }

  size_type FileImpl::getNamespaceEndOffset(char ch)
  {
    log_debug("getNamespaceEndOffset(" << ch << ')');

    size_type lower = 0;
    size_type upper = getCountArticles();
    log_debug("namespace " << ch << " lower=" << lower << " upper=" << upper);
    while (upper - lower > 1)
    {
      size_type m = lower + (upper - lower) / 2;
      Dirent d = getDirent(m);
      if (d.getNamespace() > ch)
        upper = m;
      else
        lower = m;
      log_debug("namespace " << d.getNamespace() << " m=" << m << " lower=" << lower << " upper=" << upper);
    }
    return upper;
  }

  std::string FileImpl::getNamespaces()
  {
    if (namespaces.empty())
    {
      Dirent d = getDirent(0);
      namespaces = d.getNamespace();

      size_type idx;
      while ((idx = getNamespaceEndOffset(d.getNamespace())) < getCountArticles())
      {
        d = getDirent(idx);
        namespaces += d.getNamespace();
      }

    }
    return namespaces;
  }

  std::string FileImpl::readData(offset_type off, size_type count)
  {
    cxxtools::MutexLock lock(mutex);
    return readDataNolock(off, count);
  }

  std::string FileImpl::readDataNolock(offset_type off, size_type count)
  {
    zenoFile.seekg(off);
    return readDataNolock(count);
  }

  std::string FileImpl::readDataNolock(size_type count)
  {
    std::string data;
    char buffer[256];
    while (count > 0)
    {
      zenoFile.read(buffer, std::min(static_cast<size_type>(sizeof(buffer)), count));
      if (!zenoFile)
        throw ZenoFileFormatError("format-error: error reading data");
      data.append(buffer, zenoFile.gcount());
      count -= zenoFile.gcount();
    }
    return data;
  }

  Dirent FileImpl::readDirentNolock(offset_type off)
  {
    //log_debug("read directory entry at offset " << off);
    zenoFile.seekg(off);
    return readDirentNolock();
  }

  Dirent FileImpl::readDirentNolock()
  {
    char header[26];
    if (!zenoFile.read(header, 26) || zenoFile.gcount() != 26)
      throw ZenoFileFormatError("format-error: can't read index-header in \"" + filename + '"');

    Dirent dirent(header);

    std::string extra;
    if (dirent.getExtraLen() > 0)
      extra = readDataNolock(dirent.getExtraLen());

    dirent.setExtra(extra);
    //log_debug("title=" << dirent.getTitle());

    return dirent;
  }

}
