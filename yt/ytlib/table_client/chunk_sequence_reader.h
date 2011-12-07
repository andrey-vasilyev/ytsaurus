﻿#pragma once

#include "common.h"
#include "reader.h"
#include "chunk_reader.h"

#include "../chunk_client/retriable_reader.h"
#include "../transaction_client/transaction.h"
#include "../misc/async_stream_state.h"

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TChunkSequenceReader
    : public IAsyncReader
{
public:
    typedef TIntrusivePtr<TChunkSequenceReader> TPtr;

    struct TConfig
        : public TConfigBase
    {
        NChunkClient::TRetriableReader::TConfig RetriableReader;
        NChunkClient::TSequentialReader::TConfig SequentialReader;

        TConfig()
        {
            Register("retriable_reader", RetriableReader);
            Register("sequential_reader", SequentialReader);

            SetDefaults();
        }
    };

    TChunkSequenceReader(
        const TConfig& config,
        const TChannel& channel,
        const NTransactionClient::TTransactionId transactionId,
        NRpc::IChannel::TPtr masterChannel,
        // ToDo: use rvalue reference.
        yvector<NChunkClient::TChunkId>& chunks,
        int startRow,
        int endRow);

    TAsyncStreamState::TAsyncResult::TPtr AsyncOpen();

    bool HasNextRow() const;

    TAsyncStreamState::TAsyncResult::TPtr AsyncNextRow();

    bool NextColumn();

    TValue GetValue();

    TColumn GetColumn() const;

    void Cancel(const Stroka& errorMessage);

private:
    void PrepareNextChunk();
    void OnNextReaderOpened(
        TAsyncStreamState::TResult result, 
        TChunkReader::TPtr reader);
    void SetCurrentChunk(TChunkReader::TPtr nextReader);
    void OnNextRow(TAsyncStreamState::TResult result);


    const TConfig Config;
    const TChannel Channel;
    const NTransactionClient::TTransactionId TransactionId;
    const yvector<NChunkClient::TChunkId> Chunks;
    const int StartRow;
    const int EndRow;

    NRpc::IChannel::TPtr MasterChannel;

    TAsyncStreamState State;

    int NextChunkIndex;
    TFuture<TChunkReader::TPtr>::TPtr NextReader;
    TChunkReader::TPtr CurrentReader;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
