#include "wire/frame.h"

#include "base/endian.h"

namespace wire {

void encode_frame(base::Buffer& out, const FrameHeader& header, base::Slice payload) {
  const base::u32 length = static_cast<base::u32>(kHeaderBytes + payload.size());
  out.append_u32_le(length);
  out.append_u16_le(static_cast<base::u16>(header.api_key));
  out.append_u16_le(header.api_version);
  out.append_u32_le(header.correlation_id);
  out.append(payload);
}

std::size_t FrameDecoder::bytes_needed(const base::Buffer& buf) const {
  if (buf.size() < kLengthFieldBytes) return kLengthFieldBytes - buf.size();
  const base::u32 length = base::load_u32_le(buf.data());
  const std::size_t total = kLengthFieldBytes + static_cast<std::size_t>(length);
  return buf.size() >= total ? 0 : total - buf.size();
}

base::Result<bool> FrameDecoder::next(base::Buffer& buf, FrameHeader* header,
                                      base::Slice* payload) {
  if (buf.size() < kLengthFieldBytes) return false;

  const base::u32 length = base::load_u32_le(buf.data());

  // Both checks happen before a single byte is allocated or copied.
  if (length < kHeaderBytes) return base::fail(base::ErrorCode::kInvalidRequest);
  if (length > max_frame_bytes_) return base::fail(base::ErrorCode::kMessageTooLarge);

  const std::size_t total = kLengthFieldBytes + static_cast<std::size_t>(length);
  if (buf.size() < total) return false;

  const base::u8* p = buf.data() + kLengthFieldBytes;
  header->api_key = static_cast<ApiKey>(base::load_u16_le(p));
  header->api_version = base::load_u16_le(p + 2);
  header->correlation_id = base::load_u32_le(p + 4);

  const std::size_t payload_size = static_cast<std::size_t>(length) - kHeaderBytes;
  *payload = base::Slice(buf.data() + kFramePrefixBytes, payload_size);
  return true;
}

void FrameDecoder::consume_frame(base::Buffer& buf) {
  if (buf.size() < kLengthFieldBytes) return;
  const base::u32 length = base::load_u32_le(buf.data());
  const std::size_t total = kLengthFieldBytes + static_cast<std::size_t>(length);
  if (buf.size() < total) return;
  buf.consume(total);
}

}  // namespace wire
