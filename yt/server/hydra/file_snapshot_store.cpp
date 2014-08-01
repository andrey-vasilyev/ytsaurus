#include "stdafx.h"
#include "file_snapshot_store.h"
#include "snapshot.h"
#include "config.h"
#include "private.h"
#include "file_helpers.h"

#include <core/misc/fs.h>
#include <core/misc/serialize.h>
#include <core/misc/checksum.h>
#include <core/misc/checkpointable_stream.h>

#include <core/logging/log.h>

#include <ytlib/hydra/hydra_manager.pb.h>

#include <util/stream/lz.h>

namespace NYT {
namespace NHydra {

using namespace NFS;
using namespace NCompression;
using namespace NHydra::NProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = HydraLogger;

////////////////////////////////////////////////////////////////////////////////

#pragma pack(push, 4)

struct TSnapshotHeader
{
    static const ui64 ExpectedSignature;

    ui64 Signature = ExpectedSignature;
    i32 SnapshotId = 0;
    ui64 CompressedLength = 0;
    ui64 UncompressedLength = 0;
    ui64 Checksum = 0;
    i32 Codec = ECodec::None;
    i32 MetaSize;

    void Validate() const
    {
        if (Signature != ExpectedSignature) {
            LOG_FATAL("Invalid signature: expected %v, found %v",
                ExpectedSignature,
                Signature);
        }
    }
};

const ui64 TSnapshotHeader::ExpectedSignature =  0x3330303053535459ull; // YTSS0003

static_assert(sizeof(TSnapshotHeader) == 44, "Binary size of TSnapshotHeader has changed.");

#pragma pack(pop)

////////////////////////////////////////////////////////////////////////////////

#pragma pack(push, 4)

// COMPAT(babenko)
struct TSnapshotHeader_0_16
{
    static const ui64 ExpectedSignature =  0x3130303053535459ull; // YTSS0001

    ui64 Signature = 0;
    i32 SnapshotId = 0;
    TEpochId Epoch;
    i32 PrevRecordCount = 0;
    ui64 DataLength = 0;
    ui64 Checksum = 0;
};

static_assert(sizeof(TSnapshotHeader_0_16) == 48, "Binary size of TSnapshotHeader_0_16 has changed.");

#pragma pack(pop)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

DECLARE_PODTYPE(NYT::NHydra::TSnapshotHeader)
DECLARE_PODTYPE(NYT::NHydra::TSnapshotHeader_0_16)

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

class TFileSnapshotReader
    : public ISnapshotReader
{
public:
    TFileSnapshotReader(
        const Stroka& fileName,
        int snapshotId,
        bool isRaw)
        : FileName_(fileName)
        , SnapshotId_(snapshotId)
        , IsRaw_(isRaw)
        , Logger(HydraLogger)
        , FacadeInput_(nullptr)
    {
        Logger.AddTag("FileName: %v", FileName_);
    }

    void Open(i64 offset)
    {
        try {
            File_.reset(new TFile(FileName_, OpenExisting | CloseOnExec));
            TFileInput input(*File_);

            ui64 signature;
            ReadPod(input, signature);
            File_->Seek(0, sSet);

            if (signature == TSnapshotHeader::ExpectedSignature) {
                ReadPod(input, Header_);

                if (Header_.SnapshotId != SnapshotId_) {
                    THROW_ERROR_EXCEPTION(
                        "Invalid snapshot id in header of %v: expected %v, got %v",
                        FileName_,
                        SnapshotId_,
                        Header_.SnapshotId);
                }

                if (Header_.CompressedLength != File_->GetLength()) {
                    THROW_ERROR_EXCEPTION(
                        "Invalid compressed length in header of %v: expected %v, got %v",
                        FileName_,
                        File_->GetLength(),
                        Header_.CompressedLength);
                }

                Meta_ = TSharedRef::Allocate(Header_.MetaSize, false);
                ReadPadded(input, Meta_);

                if (IsRaw_) {
                    File_->Seek(offset, sSet);
                }

                RawInput_.reset(new TBufferedFileInput(*File_));

                auto codec = ECodec(Header_.Codec);
                if (IsRaw_ || codec == ECodec::None) {
                    FacadeInput_ = CreateFakeCheckpointableInputStream(RawInput_.get());
                } else {
                    switch (codec) {
                        case ECodec::Snappy:
                            CodecInput_.reset(new TSnappyDecompress(RawInput_.get()));
                            break;
                        case ECodec::Lz4:
                            CodecInput_.reset(new TLz4Decompress(RawInput_.get()));
                            break;
                        default:
                            YUNREACHABLE();
                    }
                    FacadeInput_ = CreateCheckpointableInputStream(CodecInput_.get());
                }
            } else if (signature == TSnapshotHeader_0_16::ExpectedSignature) {
                TSnapshotHeader_0_16 legacyHeader;
                ReadPod(input, legacyHeader);

                Header_.SnapshotId = legacyHeader.SnapshotId;
                if (Header_.SnapshotId != SnapshotId_) {
                    THROW_ERROR_EXCEPTION(
                        "Invalid snapshot id in header of %v: expected %v, got %v",
                        FileName_,
                        SnapshotId_,
                        Header_.SnapshotId);
                }

                Header_.CompressedLength = legacyHeader.DataLength + sizeof (legacyHeader);
                if (Header_.CompressedLength != File_->GetLength()) {
                    THROW_ERROR_EXCEPTION(
                        "Invalid compressed length in header of %v: expected %v, got %v",
                        ~FileName_.Quote(),
                        File_->GetLength(),
                        Header_.CompressedLength);
                }

                TSnapshotMeta meta;
                meta.set_prev_record_count(legacyHeader.PrevRecordCount);
                YCHECK(SerializeToProto(meta, &Meta_));

                Header_.Checksum = legacyHeader.Checksum;
                Header_.UncompressedLength = Header_.CompressedLength;
                Header_.Codec = ECodec::Snappy;
                Header_.MetaSize = Meta_.Size();

                if (IsRaw_) {
                    File_->Seek(offset, sSet);
                }

                RawInput_.reset(new TBufferedFileInput(*File_));

                if (IsRaw_) {
                    FacadeInput_ = CreateFakeCheckpointableInputStream(RawInput_.get());
                } else {
                    CodecInput_.reset(new TSnappyDecompress(RawInput_.get()));
                    FacadeInput_ = CreateFakeCheckpointableInputStream(CodecInput_.get());
                }
            } else {
                THROW_ERROR_EXCEPTION("Unrecognized snapshot signature %" PRIx64,
                    signature);
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error opening snapshot %v for reading",
                FileName_)
                << ex;
        }
    }

    virtual ICheckpointableInputStream* GetStream() override
    {
        return FacadeInput_.get();
    }

    virtual TSnapshotParams GetParams() const override
    {
        TSnapshotParams params;
        params.Meta = Meta_;
        params.Checksum = Header_.Checksum;
        params.CompressedLength = Header_.CompressedLength;
        params.UncompressedLength = Header_.UncompressedLength;
        return params;
    }

private:
    Stroka FileName_;
    int SnapshotId_;
    bool IsRaw_;

    NLog::TLogger Logger;

    std::unique_ptr<TFile> File_;
    std::unique_ptr<TBufferedFileInput> RawInput_;
    std::unique_ptr<TInputStream> CodecInput_;
    std::unique_ptr<ICheckpointableInputStream> FacadeInput_;

    TSnapshotHeader Header_;
    TSharedRef Meta_;

};

ISnapshotReaderPtr CreateFileSnapshotReader(
    const Stroka& fileName,
    int snapshotId,
    bool isRaw,
    i64 offset)
{
    auto reader = New<TFileSnapshotReader>(
        fileName,
        snapshotId,
        isRaw);
    reader->Open(offset);
    return reader;
}

////////////////////////////////////////////////////////////////////////////////

class TFileSnapshotWriter
    : public ISnapshotWriter
{
public:
    TFileSnapshotWriter(
        const Stroka& fileName,
        ECodec codec,
        int snapshotId,
        const TSharedRef& meta,
        bool isRaw)
        : FileName_(fileName)
        , Codec_(codec)
        , SnapshotId_(snapshotId)
        , Meta_(meta)
        , IsRaw_(isRaw)
        , FacadeOutput_(nullptr)
    { }

    void Open()
    {
        // NB: Avoid logging here, this might be the forked child process.

        try {
            File_.reset(new TFile(FileName_, CreateAlways | CloseOnExec));

            RawOutput_.reset(new TBufferedFileOutput(*File_));

            if (IsRaw_) {
                FacadeOutput_ = CreateFakeCheckpointableOutputStream(RawOutput_.get());
            } else {
                TSnapshotHeader header;
                WritePod(*File_, header);
                WritePadded(*File_, Meta_);
                File_->Flush();

                ChecksumOutput_.reset(new TChecksumOutput(RawOutput_.get()));

                if (Codec_ == ECodec::None) {
                    LengthMeasureOutput_.reset(new TLengthMeasureOutputStream(ChecksumOutput_.get()));
                } else {
                    switch (Codec_) {
                        case ECodec::Snappy:
                            CodecOutput_.reset(new TSnappyCompress(ChecksumOutput_.get()));
                            break;
                        case ECodec::Lz4:
                            CodecOutput_.reset(new TLz4Compress(ChecksumOutput_.get()));
                            break;
                        default:
                            YUNREACHABLE();
                    }
                    LengthMeasureOutput_.reset(new TLengthMeasureOutputStream(CodecOutput_.get()));
                }
                FacadeOutput_ = CreateCheckpointableOutputStream(LengthMeasureOutput_.get());
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error opening snapshot %v for writing",
                FileName_)
                << ex;
        }
    }

    virtual ICheckpointableOutputStream* GetStream() override
    {
        return FacadeOutput_.get();
    }

    virtual void Close() override
    {
        // NB: Avoid logging here, this might be the forked child process.

        // NB: Some calls might be redundant.
        FacadeOutput_->Finish();
        if (LengthMeasureOutput_) {
            LengthMeasureOutput_->Finish();
        }
        if (CodecOutput_) {
            CodecOutput_->Finish();
        }
        if (ChecksumOutput_) {
            ChecksumOutput_->Finish();
        }
        RawOutput_->Finish();

        if (!IsRaw_) {
            TSnapshotHeader header;
            header.SnapshotId = SnapshotId_;
            header.CompressedLength = File_->GetLength();
            header.UncompressedLength = LengthMeasureOutput_->GetLength();
            header.Checksum = ChecksumOutput_->GetChecksum();
            header.Codec = Codec_;
            header.MetaSize = Meta_.Size();
            File_->Seek(0, sSet);
            WritePod(*File_, header);
        }

        File_->Flush();
        File_->Close();
    }

private:
    Stroka FileName_;
    ECodec Codec_;
    int SnapshotId_;
    TSharedRef Meta_;
    bool IsRaw_;

    std::unique_ptr<TFile> File_;
    std::unique_ptr<TBufferedFileOutput> RawOutput_;
    std::unique_ptr<TOutputStream> CodecOutput_;
    std::unique_ptr<TChecksumOutput> ChecksumOutput_;
    std::unique_ptr<TLengthMeasureOutputStream> LengthMeasureOutput_;
    std::unique_ptr<ICheckpointableOutputStream> FacadeOutput_;

};

ISnapshotWriterPtr CreateFileSnapshotWriter(
    const Stroka& fileName,
    ECodec codec,
    int snapshotId,
    const TSharedRef& meta,
    bool isRaw)
{
    auto writer = New<TFileSnapshotWriter>(
        fileName,
        codec,
        snapshotId,
        meta,
        isRaw);
    writer->Open();
    return writer;
}

////////////////////////////////////////////////////////////////////////////////

class TFileSnapshotStore::TImpl
    : public TRefCounted
{
public:
    explicit TImpl(TLocalSnapshotStoreConfigPtr config)
        : Config_(config)
        , Logger(HydraLogger)
    {
        Logger.AddTag("Path: %v", Config_->Path);
    }

    void Initialize()
    {
        auto path = Config_->Path;
        
        LOG_INFO("Preparing snapshot directory");
        
        NFS::ForcePath(path);
        NFS::CleanTempFiles(path);
        
        LOG_INFO("Looking for snapshots");
        
        auto fileNames = EnumerateFiles(path);
        for (const auto& fileName : fileNames) {
            auto extension = NFS::GetFileExtension(fileName);
            if (extension == SnapshotExtension) {
                auto name = NFS::GetFileNameWithoutExtension(fileName);
                try {
                    int snapshotId = FromString<int>(name);
                    RegisterSnapshot(snapshotId);
                } catch (const std::exception&) {
                    LOG_WARNING("Found unrecognized file %v", fileName);
                }
            }
        }
        
        LOG_INFO("Snapshot scan complete");
    }

    bool CheckSnapshotExists(int snapshotId)
    {
        auto path = GetSnapshotPath(snapshotId);
        if (NFS::Exists(path)) {
            return true;
        }

        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (SnapshotIds_.erase(snapshotId) == 1) {
                LOG_WARNING("Erased orphaned snapshot %v from store", snapshotId);
            }
        }

        return false;
    }

    ISnapshotReaderPtr CreateReader(int snapshotId)
    {
        if (!CheckSnapshotExists(snapshotId)) {
            THROW_ERROR_EXCEPTION("No such snapshot %v", snapshotId);
        }

        return CreateFileSnapshotReader(
            GetSnapshotPath(snapshotId),
            snapshotId,
            false);
    }

    ISnapshotReaderPtr CreateRawReader(int snapshotId, i64 offset)
    {
        return CreateFileSnapshotReader(
            GetSnapshotPath(snapshotId),
            snapshotId,
            true,
            offset);
    }

    ISnapshotWriterPtr CreateWriter(int snapshotId, const TSharedRef& meta)
    {
        return CreateFileSnapshotWriter(
            GetSnapshotPath(snapshotId) + TempFileSuffix,
            Config_->Codec,
            snapshotId,
            meta,
            false);
    }

    ISnapshotWriterPtr CreateRawWriter(int snapshotId)
    {
        return CreateFileSnapshotWriter(
            GetSnapshotPath(snapshotId) + TempFileSuffix,
            Config_->Codec,
            snapshotId,
            TSharedRef(),
            true);
    }

    int GetLatestSnapshotId(int maxSnapshotId)
    {
        TGuard<TSpinLock> guard(SpinLock_);

        auto it = SnapshotIds_.upper_bound(maxSnapshotId);
        if (it == SnapshotIds_.begin()) {
            return NonexistingSegmentId;
        }

        int snapshotId = *(--it);
        YCHECK(snapshotId <= maxSnapshotId);
        return snapshotId;
    }

    void ConfirmSnapshot(int snapshotId)
    {
        auto path = GetSnapshotPath(snapshotId);
        NFS::Rename(path + TempFileSuffix, path);
        RegisterSnapshot(snapshotId);
    }

private:
    TLocalSnapshotStoreConfigPtr Config_;

    NLog::TLogger Logger;

    TSpinLock SpinLock_;
    std::set<int> SnapshotIds_;



    Stroka GetSnapshotPath(int snapshotId)
    {
        return NFS::CombinePaths(
            Config_->Path,
            Format("%09d.%v", snapshotId, SnapshotExtension));
    }

    void RegisterSnapshot(int snapshotId)
    {
        TGuard<TSpinLock> guard(SpinLock_);
        YCHECK(SnapshotIds_.insert(snapshotId).second);
        LOG_INFO("Registered snapshot %v", snapshotId);
    }

};

////////////////////////////////////////////////////////////////////////////////

TFileSnapshotStore::TFileSnapshotStore(TLocalSnapshotStoreConfigPtr config)
    : Impl_(New<TImpl>(config))
{ }

TFileSnapshotStore::~TFileSnapshotStore()
{ }

void TFileSnapshotStore::Initialize()
{
    Impl_->Initialize();
}

bool TFileSnapshotStore::CheckSnapshotExists(int snapshotId)
{
    return Impl_->CheckSnapshotExists(snapshotId);
}

ISnapshotReaderPtr TFileSnapshotStore::CreateReader(int snapshotId)
{
    return Impl_->CreateReader(snapshotId);
}

ISnapshotReaderPtr TFileSnapshotStore::CreateRawReader(int snapshotId, i64 offset)
{
    return Impl_->CreateRawReader(snapshotId, offset);
}

ISnapshotWriterPtr TFileSnapshotStore::CreateWriter(int snapshotId, const TSharedRef& meta)
{
    return Impl_->CreateWriter(snapshotId, meta);
}

ISnapshotWriterPtr TFileSnapshotStore::CreateRawWriter(int snapshotId)
{
    return Impl_->CreateRawWriter(snapshotId);
}

int TFileSnapshotStore::GetLatestSnapshotId(int maxSnapshotId)
{
    return Impl_->GetLatestSnapshotId(maxSnapshotId);
}

void TFileSnapshotStore::ConfirmSnapshot(int snapshotId)
{
    Impl_->ConfirmSnapshot(snapshotId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
