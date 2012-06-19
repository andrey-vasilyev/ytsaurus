#include "stdafx.h"
#include "snapshot.h"
#include "common.h"

#include <ytlib/misc/fs.h>
#include <ytlib/misc/common.h>
#include <ytlib/misc/serialize.h>

#include <util/stream/lz.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

namespace {

typedef TSnappyCompress TCompressedOutput;
typedef TSnappyDecompress TDecompressedInput;

template<class TOutput>
void AppendZeroes(TOutput& output, size_t count)
{
    std::vector<char> zeroes(count, 0);
    output.Append(&(*zeroes.begin()), count);
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;

////////////////////////////////////////////////////////////////////////////////

#pragma pack(push, 4)

struct TSnapshotHeader
{
    static const ui64 CorrectSignature =  0x3130303053535459ull; // YTSS0002

    ui64 Signature;
    i32 SegmentId;
    TEpoch Epoch;
    i32 PrevRecordCount;
    ui64 DataLength;
    ui64 Checksum;

    TSnapshotHeader()
        : Signature(CorrectSignature)
        , SegmentId(0)
        , Epoch()
        , PrevRecordCount(0)
        , DataLength(0)
        , Checksum(0)
    { }

    void Validate() const
    {
        if (Signature != CorrectSignature) {
            LOG_FATAL("Invalid signature: expected %" PRIx64 ", found %" PRIx64,
                CorrectSignature,
                Signature);
        }
    }
};

static_assert(sizeof(TSnapshotHeader) == 48, "Binary size of TSnapshotHeader has changed.");

#pragma pack(pop)

////////////////////////////////////////////////////////////////////////////////

TSnapshotReader::TSnapshotReader(
    const Stroka& fileName,
    i32 segmentId,
    bool enableCompression)
    : FileName(fileName)
    , SnapshotId(segmentId)
    , EnableCompression(enableCompression)
{ }

void TSnapshotReader::Open()
{
    LOG_DEBUG("Opening snapshot reader %s", ~FileName);

    File.Reset(new TFile(FileName, OpenExisting));

    Header.Reset(new TSnapshotHeader());
    ReadPod(*File, *Header);

    Header->Validate();
    LOG_FATAL_UNLESS(Header->SegmentId == SnapshotId,
        "Invalid snapshot id in header: expected %d, got %d", SnapshotId, Header->SegmentId);
    YASSERT(Header->DataLength + sizeof(*Header) == static_cast<ui64>(File->GetLength()));

    FileInput.Reset(new TBufferedFileInput(*File));
    TInputStream* inputStream = ~FileInput;
    if (EnableCompression) {
        DecompressedInput.Reset(new TDecompressedInput(inputStream));
        inputStream = ~DecompressedInput;
    }
    ChecksummableInput.Reset(new TChecksummableInput(inputStream));
}

TInputStream* TSnapshotReader::GetStream() const
{
    YASSERT(~ChecksummableInput);
    return ~ChecksummableInput;
}

i64 TSnapshotReader::GetLength() const
{
    YASSERT(~File);
    return File->GetLength();
}

TChecksum TSnapshotReader::GetChecksum() const
{
    YASSERT(~Header);
    return Header->Checksum;
}

i32 TSnapshotReader::GetPrevRecordCount() const
{
    YASSERT(~Header);
    return Header->PrevRecordCount;
}

const TEpoch& TSnapshotReader::GetEpoch() const
{
    YASSERT(~Header);
    return Header->Epoch;
}

////////////////////////////////////////////////////////////////////////////////

TSnapshotWriter::TSnapshotWriter(
    const Stroka& fileName,
    i32 segmentId,
    bool enableCompression)
    : State(EState::Uninitialized)
    , FileName(fileName)
    , TempFileName(fileName + NFS::TempFileSuffix)
    , EnableCompression(enableCompression)
    , Header(new TSnapshotHeader())
{
    Header->SegmentId = segmentId;
}

void TSnapshotWriter::Open(i32 prevRecordCount, const TEpoch& epoch)
{
    YASSERT(State == EState::Uninitialized);

    Header->PrevRecordCount = prevRecordCount;
    Header->Epoch = epoch;

    File.Reset(new TBufferedFile(TempFileName, RdWr | CreateAlways));
    AppendZeroes(*File, sizeof(TSnapshotHeader));

    TOutputStream* outputStream = File->GetOutputStream();
    if (EnableCompression) {
        CompressedOutput.Reset(new TCompressedOutput(outputStream));
        outputStream = ~CompressedOutput;
    }
    ChecksummableOutput.Reset(new TChecksummableOutput(outputStream));

    State = EState::Opened;
}

TOutputStream* TSnapshotWriter::GetStream() const
{
    YASSERT(State == EState::Opened);
    return ~ChecksummableOutput;
}

void TSnapshotWriter::Close()
{
    if (State != EState::Opened) {
        return;
    }

    Header->Checksum = ChecksummableOutput->GetChecksum();
    Header->DataLength = File->GetLength() - sizeof(TSnapshotHeader);

    File->Seek(0, sSet);
    WritePod(*File, *Header);
    File->Close();

    Move(TempFileName, FileName);
}

TChecksum TSnapshotWriter::GetChecksum() const
{
    YASSERT(State == EState::Closed);
    return Header->Checksum;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
