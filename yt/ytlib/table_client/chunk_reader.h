#pragma once

#include "public.h"
#include "schema.h"
#include "async_reader.h"
#include "key.h"

#include <ytlib/table_client/table_reader.pb.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/ytree/public.h>
#include <ytlib/misc/codec.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/async_stream_state.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Reads single table chunk row-after-row using given #NChunkClient::IAsyncReader.
class TChunkReader
    : public IAsyncReader
{
public:
    struct TOptions
    {
        bool ReadKey;

        TOptions()
            : ReadKey(false)
        { }
    };

    /*! 
     *  \param EndRow - if given value exceeds row count of the chunk,
     *  chunk is processed to the end without error. To guarantee reading
     *  chunk to the end, set it to numerical_limits<int>::max().
     */
    TChunkReader(
        NChunkClient::TSequentialReaderConfigPtr config,
        const TChannel& channel,
        NChunkClient::IAsyncReaderPtr chunkReader,
        const NProto::TReadLimit& startLimit,
        const NProto::TReadLimit& endLimit,
        const NYTree::TYson& rowAttributes,
        TOptions options = TOptions());

    TAsyncError AsyncOpen();

    TAsyncError AsyncNextRow();
    bool IsValid() const;

    TRow& GetRow();
    TKey& GetKey();
    const NYTree::TYson& GetRowAttributes() const;

private:
    class TKeyValidator;

    ICodec* Codec;
    NChunkClient::TSequentialReaderPtr SequentialReader;
    TChannel Channel;

    TAsyncError DoNextRow();

    // TODO(babenko): performance critical, use const TError&
    void OnNextRowFetched(TError error);
    TCallback<void(TError)> OnNextRowFetched_;

    TAsyncError ContinueNextRow(
        int channelIndex, 
        TAsyncErrorPromise result,
        TError error);

    void MakeCurrentRow();

    class TInitializer;
    TIntrusivePtr<TInitializer> Initializer;

    TAsyncStreamState State;
    TOptions Options;

    NYTree::TYson RowAttributes;
    TRow CurrentRow;
    TKey CurrentKey;

    struct TColumnInfo
    {
        int KeyIndex;
        bool InChannel;
        bool Used;

        TColumnInfo()
            : KeyIndex(-1)
            , InChannel(false)
            , Used(false)
        { }
    };

    yhash_map<TStringBuf, TColumnInfo> FixedColumns;
    yhash_set<TStringBuf> UsedRangeColumns;

    i64 CurrentRowIndex;
    i64 EndRowIndex;

    THolder<TKeyValidator> EndValidator;

    TAutoPtr<NProto::TKeyColumns> KeyColumns;

    /*! 
     *  See DoNextRow for usage.
     */
    const TAsyncErrorPromise SuccessResult;

    std::vector<TChannelReaderPtr> ChannelReaders;

    DECLARE_THREAD_AFFINITY_SLOT(ClientThread);
    DECLARE_THREAD_AFFINITY_SLOT(ReaderThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
