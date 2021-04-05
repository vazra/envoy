#include "common/quic/envoy_quic_server_stream.h"

#include <openssl/bio.h>
#include <openssl/evp.h>

#include <memory>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#include "quiche/quic/core/http/quic_header_list.h"
#include "quiche/quic/core/quic_session.h"
#include "quiche/spdy/core/spdy_header_block.h"
#include "common/quic/platform/quic_mem_slice_span_impl.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "common/quic/envoy_quic_utils.h"
#include "common/quic/envoy_quic_server_session.h"

#include "common/buffer/buffer_impl.h"
#include "common/http/header_map_impl.h"
#include "common/common/assert.h"
#include "common/http/header_utility.h"

namespace Envoy {
namespace Quic {

EnvoyQuicServerStream::EnvoyQuicServerStream(quic::QuicStreamId id, quic::QuicSpdySession* session,
                                             quic::StreamType type)
    : quic::QuicSpdyServerStreamBase(id, session, type),
      EnvoyQuicStream(
          // Flow control receive window should be larger than 8k to fully utilize congestion
          // control window before it reaches the high watermark.
          static_cast<uint32_t>(GetReceiveWindow().value()), *filterManagerConnection(),
          [this]() { runLowWatermarkCallbacks(); }, [this]() { runHighWatermarkCallbacks(); }) {
  ASSERT(GetReceiveWindow() > 8 * 1024, "Send buffer limit should be larger than 8KB.");
}

EnvoyQuicServerStream::EnvoyQuicServerStream(quic::PendingStream* pending,
                                             quic::QuicSpdySession* session, quic::StreamType type)
    : quic::QuicSpdyServerStreamBase(pending, session, type),
      EnvoyQuicStream(
          // This should be larger than 8k to fully utilize congestion control
          // window. And no larger than the max stream flow control window for
          // the stream to buffer all the data.
          static_cast<uint32_t>(GetReceiveWindow().value()), *filterManagerConnection(),
          [this]() { runLowWatermarkCallbacks(); }, [this]() { runHighWatermarkCallbacks(); }) {}

void EnvoyQuicServerStream::encode100ContinueHeaders(const Http::ResponseHeaderMap& headers) {
  ASSERT(headers.Status()->value() == "100");
  encodeHeaders(headers, false);
}

void EnvoyQuicServerStream::encodeHeaders(const Http::ResponseHeaderMap& headers, bool end_stream) {
  ENVOY_STREAM_LOG(debug, "encodeHeaders (end_stream={}) {}.", *this, end_stream, headers);
  // QUICHE guarantees to take all the headers. This could cause infinite data to
  // be buffered on headers stream in Google QUIC implementation because
  // headers stream doesn't have upper bound for its send buffer. But in IETF
  // QUIC implementation this is safe as headers are sent on data stream which
  // is bounded by max concurrent streams limited.
  // Same vulnerability exists in crypto stream which can infinitely buffer data
  // if handshake implementation goes wrong.
  // TODO(#8826) Modify QUICHE to have an upper bound for header stream send buffer.
  // This is counting not serialized bytes in the send buffer.
  local_end_stream_ = end_stream;
  SendBufferMonitor::ScopedWatermarkBufferUpdater updater(this, this);
  WriteHeaders(envoyHeadersToSpdyHeaderBlock(headers), end_stream, nullptr);
}

void EnvoyQuicServerStream::encodeData(Buffer::Instance& data, bool end_stream) {
  ENVOY_STREAM_LOG(debug, "encodeData (end_stream={}) of {} bytes.", *this, end_stream,
                   data.length());
  if (data.length() == 0 && !end_stream) {
    return;
  }
  ASSERT(!local_end_stream_);
  local_end_stream_ = end_stream;
  SendBufferMonitor::ScopedWatermarkBufferUpdater updater(this, this);
  // QUIC stream must take all.
  WriteBodySlices(quic::QuicMemSliceSpan(quic::QuicMemSliceSpanImpl(data)), end_stream);
  if (data.length() > 0) {
    // Send buffer didn't take all the data, threshold needs to be adjusted.
    Reset(quic::QUIC_BAD_APPLICATION_PAYLOAD);
    return;
  }
}

void EnvoyQuicServerStream::encodeTrailers(const Http::ResponseTrailerMap& trailers) {
  ASSERT(!local_end_stream_);
  local_end_stream_ = true;
  ENVOY_STREAM_LOG(debug, "encodeTrailers: {}.", *this, trailers);
  SendBufferMonitor::ScopedWatermarkBufferUpdater updater(this, this);
  WriteTrailers(envoyHeadersToSpdyHeaderBlock(trailers), nullptr);
}

void EnvoyQuicServerStream::encodeMetadata(const Http::MetadataMapVector& /*metadata_map_vector*/) {
  // Metadata Frame is not supported in QUIC.
  // TODO(danzh): add stats for metadata not supported error.
}

void EnvoyQuicServerStream::resetStream(Http::StreamResetReason reason) {
  if (local_end_stream_ && !reading_stopped()) {
    // This is after 200 early response. Reset with QUIC_STREAM_NO_ERROR instead
    // of propagating original reset reason. In QUICHE if a stream stops reading
    // before FIN or RESET received, it resets the steam with QUIC_STREAM_NO_ERROR.
    StopReading();
    runResetCallbacks(Http::StreamResetReason::LocalReset);
  } else {
    Reset(envoyResetReasonToQuicRstError(reason));
  }
}

void EnvoyQuicServerStream::switchStreamBlockState(bool should_block) {
  ASSERT(FinishedReadingHeaders(),
         "Upperstream buffer limit is reached before request body is delivered.");
  if (should_block) {
    sequencer()->SetBlockedUntilFlush();
  } else {
    ASSERT(read_disable_counter_ == 0, "readDisable called in between.");
    sequencer()->SetUnblocked();
  }
}

void EnvoyQuicServerStream::OnInitialHeadersComplete(bool fin, size_t frame_len,
                                                     const quic::QuicHeaderList& header_list) {
  // TODO(danzh) Fix in QUICHE. If the stream has been reset in the call stack,
  // OnInitialHeadersComplete() shouldn't be called.
  if (read_side_closed()) {
    return;
  }
  quic::QuicSpdyServerStreamBase::OnInitialHeadersComplete(fin, frame_len, header_list);
  ASSERT(headers_decompressed() && !header_list.empty());
  ENVOY_STREAM_LOG(debug, "Received headers: {}.", *this, header_list.DebugString());
  if (fin) {
    end_stream_decoded_ = true;
  }
  std::unique_ptr<Http::RequestHeaderMapImpl> headers =
      quicHeadersToEnvoyHeaders<Http::RequestHeaderMapImpl>(header_list);
  if (Http::HeaderUtility::requestHeadersValid(*headers) != absl::nullopt) {
    stream_delegate()->OnStreamError(quic::QUIC_HTTP_FRAME_ERROR, "Invalid headers");
    return;
  }
  request_decoder_->decodeHeaders(std::move(headers),
                                  /*end_stream=*/fin);
  ConsumeHeaderList();
}

void EnvoyQuicServerStream::OnBodyAvailable() {
  ASSERT(FinishedReadingHeaders());
  ASSERT(read_disable_counter_ == 0);
  ASSERT(!in_decode_data_callstack_);
  if (read_side_closed()) {
    return;
  }
  in_decode_data_callstack_ = true;

  Buffer::InstancePtr buffer = std::make_unique<Buffer::OwnedImpl>();
  // TODO(danzh): check Envoy per stream buffer limit.
  // Currently read out all the data.
  while (HasBytesToRead()) {
    iovec iov;
    int num_regions = GetReadableRegions(&iov, 1);
    ASSERT(num_regions > 0);
    size_t bytes_read = iov.iov_len;
    buffer->add(iov.iov_base, bytes_read);
    MarkConsumed(bytes_read);
  }

  bool fin_read_and_no_trailers = IsDoneReading();
  ENVOY_STREAM_LOG(debug, "Received {} bytes of data {} FIN.", *this, buffer->length(),
                   fin_read_and_no_trailers ? "with" : "without");
  // If this call is triggered by an empty frame with FIN which is not from peer
  // but synthesized by stream itself upon receiving HEADERS with FIN or
  // TRAILERS, do not deliver end of stream here. Because either decodeHeaders
  // already delivered it or decodeTrailers will be called.
  bool skip_decoding = (buffer->length() == 0 && !fin_read_and_no_trailers) || end_stream_decoded_;
  if (!skip_decoding) {
    if (fin_read_and_no_trailers) {
      end_stream_decoded_ = true;
    }
    request_decoder_->decodeData(*buffer, fin_read_and_no_trailers);
  }

  if (!sequencer()->IsClosed() || read_side_closed()) {
    in_decode_data_callstack_ = false;
    if (read_disable_counter_ > 0) {
      // If readDisable() was ever called during decodeData() and it meant to disable
      // reading from downstream, the call must have been deferred. Call it now.
      switchStreamBlockState(true);
    }
    return;
  }

  // Trailers may arrived earlier and wait to be consumed after reading all the body. Consume it
  // here.
  maybeDecodeTrailers();

  OnFinRead();
  in_decode_data_callstack_ = false;
}

void EnvoyQuicServerStream::OnTrailingHeadersComplete(bool fin, size_t frame_len,
                                                      const quic::QuicHeaderList& header_list) {
  if (read_side_closed()) {
    return;
  }
  ENVOY_STREAM_LOG(debug, "Received trailers: {}.", *this, received_trailers().DebugString());
  quic::QuicSpdyServerStreamBase::OnTrailingHeadersComplete(fin, frame_len, header_list);
  ASSERT(trailers_decompressed());
  if (session()->connection()->connected() && !rst_sent()) {
    maybeDecodeTrailers();
  }
}

void EnvoyQuicServerStream::OnHeadersTooLarge() {
  ENVOY_STREAM_LOG(debug, "Headers too large.", *this);
  quic::QuicSpdyServerStreamBase::OnHeadersTooLarge();
}

void EnvoyQuicServerStream::maybeDecodeTrailers() {
  if (sequencer()->IsClosed() && !FinishedReadingTrailers()) {
    ASSERT(!received_trailers().empty());
    // Only decode trailers after finishing decoding body.
    end_stream_decoded_ = true;
    request_decoder_->decodeTrailers(
        spdyHeaderBlockToEnvoyHeaders<Http::RequestTrailerMapImpl>(received_trailers()));
    MarkTrailersConsumed();
  }
}

bool EnvoyQuicServerStream::OnStopSending(quic::QuicRstStreamErrorCode error) {
  bool ret = quic::QuicSpdyServerStreamBase::OnStopSending(error);
  if (read_side_closed()) {
    // Treat this as a remote reset, since the stream will be closed in both directions.
    runResetCallbacks(quicRstErrorToEnvoyRemoteResetReason(error));
  }
  return ret;
}

void EnvoyQuicServerStream::OnStreamReset(const quic::QuicRstStreamFrame& frame) {
  quic::QuicSpdyServerStreamBase::OnStreamReset(frame);
  runResetCallbacks(quicRstErrorToEnvoyRemoteResetReason(frame.error_code));
}

void EnvoyQuicServerStream::Reset(quic::QuicRstStreamErrorCode error) {
  // Upper layers expect calling resetStream() to immediately raise reset callbacks.
  runResetCallbacks(quicRstErrorToEnvoyLocalResetReason(error));
  quic::QuicSpdyServerStreamBase::Reset(error);
}

void EnvoyQuicServerStream::OnConnectionClosed(quic::QuicErrorCode error,
                                               quic::ConnectionCloseSource source) {
  // Run reset callback before closing the stream so that the watermark change will not trigger
  // callbacks.
  if (!local_end_stream_) {
    runResetCallbacks(quicErrorCodeToEnvoyResetReason(error));
  }
  quic::QuicSpdyServerStreamBase::OnConnectionClosed(error, source);
}

void EnvoyQuicServerStream::OnClose() {
  quic::QuicSpdyServerStreamBase::OnClose();
  if (isDoingWatermarkAccounting()) {
    connection()->dispatcher().post([this] { clearWatermarkBuffer(); });
    return;
  }
  clearWatermarkBuffer();
}

void EnvoyQuicServerStream::clearWatermarkBuffer() {
  if (BufferedDataBytes() > 0) {
    // If the stream is closed without sending out all buffered data, regard
    // them as sent now and adjust connection buffer book keeping.
    updateBytesBuffered(BufferedDataBytes(), 0);
  }
}

void EnvoyQuicServerStream::OnCanWrite() {
  SendBufferMonitor::ScopedWatermarkBufferUpdater updater(this, this);
  quic::QuicSpdyServerStreamBase::OnCanWrite();
}

uint32_t EnvoyQuicServerStream::streamId() { return id(); }

Network::Connection* EnvoyQuicServerStream::connection() { return filterManagerConnection(); }

QuicFilterManagerConnectionImpl* EnvoyQuicServerStream::filterManagerConnection() {
  return dynamic_cast<QuicFilterManagerConnectionImpl*>(session());
}

} // namespace Quic
} // namespace Envoy
