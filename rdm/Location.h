#ifndef Location_h
#define Location_h

#include <ByteArray.h>
#include <QReadWriteLock>
#include <Path.h>
#include <Log.h>
#include <stdio.h>
#include <assert.h>
#include "RTags.h"
#include <clang-c/Index.h>

class Location
{
public:
    Location()
        : mData(0)
    {}
    Location(quint64 data)
        : mData(data)
    {}
    Location(quint32 fileId, quint32 offset)
        : mData(quint64(offset) << 32 | fileId)
    {}

    Location(const CXFile &file, quint32 offset)
        : mData(0)
    {
        Q_ASSERT(file);
        CXString fn = clang_getFileName(file);
        const char *cstr = clang_getCString(fn);
        if (!cstr)
            return;
        const Path p = Path::canonicalized(cstr);
        clang_disposeString(fn);
        quint32 fileId = insertFile(p);
        mData = (quint64(offset) << 32) | fileId;
    }
    Location(const CXSourceLocation &location)
        : mData(0)
    {
        CXFile file;
        unsigned offset;
        clang_getSpellingLocation(location, &file, 0, 0, &offset);
        *this = Location(file, offset);
    }
    static inline quint32 fileId(const Path &path)
    {
        QReadLocker lock(&sLock);
        return sPathsToIds.value(path);
    }
    static inline Path path(quint32 id)
    {
        QReadLocker lock(&sLock);
        return sIdsToPaths.value(id);
    }

    static inline quint32 insertFile(const Path &path)
    {
        bool newFile = false;
        quint32 ret;
        {
            QWriteLocker lock(&sLock);
            quint32 &id = sPathsToIds[path];
            if (!id) {
                id = ++sLastId;
                sIdsToPaths[id] = path;
                newFile = true;
            }
            ret = id;
        }
        if (newFile)
            writeToDB(path, ret);

        return ret;
    }
    static void writeToDB(const Path &path, quint32 file);
    static void init(const QHash<Path, quint32> &pathsToIds,
                     const QHash<quint32, Path> &idsToPaths,
                     quint32 maxId)
    {
        sPathsToIds = pathsToIds;
        sIdsToPaths = idsToPaths;
        sLastId = maxId;
    }

    inline quint32 fileId() const { return quint32(mData); }
    inline quint32 offset() const { return quint32(mData >> 32); }

    inline Path path() const
    {
        if (mCachedPath.isEmpty()) {
            QReadLocker lock(&sLock);
            mCachedPath = sIdsToPaths.value(fileId());
        }
        return mCachedPath;
    }
    inline bool isNull() const { return !mData; }
    inline bool isValid() const { return mData; }
    inline void clear() { mData = 0; mCachedPath.clear(); }
    inline bool operator==(const Location &other) const { return mData == other.mData; }
    inline bool operator!=(const Location &other) const { return mData != other.mData; }
    inline bool operator<(const Location &other) const
    {
        const int off = other.fileId() - fileId();
        if (off < 0) {
            return true;
        } else if (off > 0) {
            return false;
        }
        return offset() < other.offset();
    }

    ByteArray context() const
    {
        const quint32 off = offset();
        quint32 o = off;
        Path p = path();
        FILE *f = fopen(p.constData(), "r");
        if (f && !fseek(f, off, SEEK_SET)) {
            while (o > 0) {
                const char ch = fgetc(f);
                if (ch == '\n' && o != off)
                    break;
                if (fseek(f, --o, SEEK_SET) == -1) {
                    fclose(f);
                    return ByteArray();
                }
            }
            char buf[1024] = { '\0' };
            const int len = RTags::readLine(f, buf, 1023);
            fclose(f);
            return ByteArray(buf, len);
        }
        if (f)
            fclose(f);
        return ByteArray();
    }

    bool convertOffset(int &line, int &col) const
    {
        const quint32 off = offset();
        Path p = path();
        FILE *f = fopen(p.constData(), "r");
        if (!f) {
            line = col = -1;
            return false;
        }
        line = 1;
        int last = 0;
        quint32 idx = 0;
        forever {
            const int lineLen = RTags::readLine(f);
            if (lineLen == -1) {
                col = line = -1;
                fclose(f);
                return false;
            }
            idx += lineLen + 1;
            // printf("lineStart %d offset %d last %d lineLen %d\n", idx, offset, last, lineLen);
            if (idx > off) {
                col = off - last + 1;
                break;
            }
            last = idx;
            ++line;
        }
        fclose(f);
        return true;
    }


    ByteArray key(unsigned flags = RTags::NoFlag) const
    {
        if (isNull())
            return ByteArray();
        int extra = 0;
        const int off = offset();
        int line = 0, col = 0;
        if (flags & RTags::Padded) {
            extra = 7;
        } else if (flags & RTags::ShowLineNumbers && convertOffset(line, col)) {
            extra = RTags::digits(line) + RTags::digits(col) + 3;
        } else {
            flags &= ~RTags::ShowLineNumbers;
            extra = RTags::digits(off) + 1;
        }
        ByteArray ctx;
        if (flags & RTags::ShowContext) {
            ctx += '\t';
            ctx += context();
            extra += ctx.size();
        }

        const Path p = path();

        ByteArray ret(p.size() + extra, '0');

        if (flags & RTags::Padded) {
            snprintf(ret.data(), ret.size() + extra + 1, "%s,%06d%s", p.constData(),
                     off, ctx.constData());
        } else if (flags & RTags::ShowLineNumbers) {
            snprintf(ret.data(), ret.size() + extra + 1, "%s:%d:%d:%s", p.constData(),
                     line, col, ctx.constData());
        } else {
            snprintf(ret.data(), ret.size() + extra + 1, "%s,%d%s", p.constData(),
                     off, ctx.constData());
        }
        return ret;
    }

    bool toKey(char buf[8]) const
    {
        if (isNull()) {
            memset(buf, 0, 8);
            return false;
        } else {
            memcpy(buf, &mData, sizeof(mData));
            return true;
        }
    }

    static Location fromKey(const char *data)
    {
        Location ret;
        memcpy(&ret.mData, data, sizeof(ret.mData));
        return ret;
    }

    static Location decodeClientLocation(const ByteArray &data)
    {
        quint32 offset;
        memcpy(&offset, data.constData() + data.size() - sizeof(offset), sizeof(offset));
        const Path path(data.constData(), data.size() - sizeof(offset));
        QReadLocker lock(&sLock);
        const quint32 fileId = sPathsToIds.value(path, 0);
        if (fileId)
            return Location(fileId, offset);
        error("Failed to make location from [%s,%d]", path.constData(), offset);
        return Location();
    }
    static Location fromPathAndOffset(const ByteArray &pathAndOffset)
    {
        const int comma = pathAndOffset.lastIndexOf(',');
        if (comma <= 0 || comma + 1 == pathAndOffset.size()) {
            error("Can't create location from this: %s", pathAndOffset.constData());
            return Location();
        }
        bool ok;
        const quint32 fileId = ByteArray(pathAndOffset.constData() + comma + 1, pathAndOffset.size() - comma - 1).toUInt(&ok);
        if (!ok) {
            error("Can't create location from this: %s", pathAndOffset.constData());
            return Location();
        }
        return Location(Location::insertFile(Path(pathAndOffset.left(comma))), fileId);
    }
quint64 mData;
private:
    static QHash<Path, quint32> sPathsToIds;
    static QHash<quint32, Path> sIdsToPaths;
    static quint32 sLastId;
    static QReadWriteLock sLock;
    mutable Path mCachedPath;
};

static inline QDataStream &operator<<(QDataStream &ds, const Location &loc)
{
    ds << loc.mData;
    return ds;
}

static inline QDataStream &operator>>(QDataStream &ds, Location &loc)
{
    ds >> loc.mData;
    return ds;
}

static inline QDebug operator<<(QDebug dbg, const Location &loc)
{
    const ByteArray out = "Location(" + loc.key() + ")";
    return (dbg << out);
}

static inline uint qHash(const Location &l)
{
    return qHash(l.mData);
}

#endif
