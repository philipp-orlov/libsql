import { decodeReply } from './reply';
import { encodeRequest } from './request';
import { REPLIES, REQUESTS } from './golden';

function decode(base64: string): Uint8Array {
  const text = atob(base64);
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) {
    bytes[i] = text.charCodeAt(i);
  }
  return bytes;
}

describe('the remote protocol against the C++ side', () => {
  for (const golden of REQUESTS) {
    it(`encodes the request ${golden.name} byte for byte`, () => {
      const got = new Uint8Array(encodeRequest(golden.fields));
      const want = decode(golden.bytes);
      expect(got.length).withContext('length').toBe(want.length);
      expect(Array.from(got)).toEqual(Array.from(want));
    });
  }

  for (const golden of REPLIES) {
    it(`decodes the reply ${golden.name}`, () => {
      const bytes = decode(golden.bytes);
      const reply = decodeReply(bytes.buffer as ArrayBuffer);
      const { size, ...fields } = reply;
      expect(size).toBe(bytes.length);
      expect(fields).toEqual(golden.fields as never);
    });
  }
});
