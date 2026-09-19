package main

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"io"
	"os"
	"strconv"
	"strings"

	"github.com/dhowden/tag"
)

const maxCoverBytes = 16 << 20
const maxCoverMetadataBytes = 64 << 20

// extractCoverArt returns only front artwork. tag.Picture() alone is not a
// selector: the tag reader keeps the last FLAC/Vorbis picture and the first ID3
// picture, either of which may be an artist photo or a back cover.
func extractCoverArt(path string) ([]byte, string) {
	f, err := os.Open(path)
	if err != nil {
		return nil, ""
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil || !info.Mode().IsRegular() {
		return nil, ""
	}
	var magic [4]byte
	if _, err = io.ReadFull(f, magic[:]); err != nil {
		return nil, ""
	}
	switch string(magic[:]) {
	case "fLaC":
		return readFLACFrontCover(io.LimitReader(f, maxCoverMetadataBytes))
	case "OggS":
		if _, err = f.Seek(0, io.SeekStart); err != nil {
			return nil, ""
		}
		return readOggFrontCover(io.LimitReader(f, maxCoverMetadataBytes))
	}
	if _, err = f.Seek(0, io.SeekStart); err != nil {
		return nil, ""
	}
	m, err := tag.ReadFrom(f)
	if err != nil {
		return nil, ""
	}
	// MP4's covr atom is dedicated cover artwork; it has no ID3 picture role.
	if m.Format() == tag.MP4 {
		return coverPictureBytes(m.Picture(), true)
	}
	raw := m.Raw()
	for _, key := range []string{"APIC", "PIC"} {
		// Duplicate ID3 frames are exposed as APIC, APIC_0, APIC_1, ... .
		// Visit them in file order, not randomized Go map order.
		for i := -1; i < len(raw); i++ {
			name := key
			if i >= 0 {
				name += "_" + strconv.Itoa(i)
			}
			value, ok := raw[name]
			if !ok {
				break
			}
			picture, _ := value.(*tag.Picture)
			if data, mime := coverPictureBytes(picture, false); data != nil {
				return data, mime
			}
		}
	}
	return nil, ""
}

func coverPictureBytes(p *tag.Picture, implicitCover bool) ([]byte, string) {
	if p == nil || (!implicitCover && p.Type != "Cover (front)") ||
		len(p.Data) == 0 || len(p.Data) > maxCoverBytes || !strings.HasPrefix(p.MIMEType, "image/") {
		return nil, ""
	}
	return p.Data, p.MIMEType
}

func readFLACFrontCover(r io.Reader) ([]byte, string) {
	for blocks := 0; blocks < 4096; blocks++ {
		var header [4]byte
		if _, err := io.ReadFull(r, header[:]); err != nil {
			return nil, ""
		}
		size := int(header[1])<<16 | int(header[2])<<8 | int(header[3])
		if header[0]&0x7f == 6 {
			payload := make([]byte, size)
			if _, err := io.ReadFull(r, payload); err != nil {
				return nil, ""
			}
			if data, mime := frontPictureBlock(payload); data != nil {
				return data, mime
			}
		} else if _, err := io.CopyN(io.Discard, r, int64(size)); err != nil {
			return nil, ""
		}
		if header[0]&0x80 != 0 {
			break
		}
	}
	return nil, ""
}

// FLAC PICTURE and Vorbis METADATA_BLOCK_PICTURE share this layout. Type 3 is
// front cover; descriptions are free text and must never override that type.
func frontPictureBlock(payload []byte) ([]byte, string) {
	if len(payload) < 4 || binary.BigEndian.Uint32(payload[:4]) != 3 {
		return nil, ""
	}
	payload = payload[4:]
	mime, ok := coverLengthField(&payload, binary.BigEndian)
	if !ok || !bytes.HasPrefix(mime, []byte("image/")) {
		return nil, ""
	}
	if _, ok = coverLengthField(&payload, binary.BigEndian); !ok || len(payload) < 16 {
		return nil, ""
	}
	payload = payload[16:] // width, height, depth, indexed colors
	data, ok := coverLengthField(&payload, binary.BigEndian)
	if !ok || len(data) == 0 || len(data) > maxCoverBytes {
		return nil, ""
	}
	return data, string(mime)
}

func coverLengthField(payload *[]byte, order binary.ByteOrder) ([]byte, bool) {
	if len(*payload) < 4 {
		return nil, false
	}
	size := uint64(order.Uint32((*payload)[:4]))
	*payload = (*payload)[4:]
	if size > uint64(len(*payload)) {
		return nil, false
	}
	value := (*payload)[:int(size)]
	*payload = (*payload)[int(size):]
	return value, true
}

// Read only the first audio stream's identification/comment packets. Preserve
// repeated picture comments instead of flattening them into the tag reader's
// map. Lacing permits a comment packet to span several Ogg pages.
func readOggFrontCover(r io.Reader) ([]byte, string) {
	var packet []byte
	var serial uint32
	var sequence uint32
	selected := false
	identified := false
	opus := false
	for pages := 0; pages < 4096; pages++ {
		var header [27]byte
		if _, err := io.ReadFull(r, header[:]); err != nil || string(header[:4]) != "OggS" || header[4] != 0 {
			return nil, ""
		}
		lacing := make([]byte, int(header[26]))
		if _, err := io.ReadFull(r, lacing); err != nil {
			return nil, ""
		}
		size := 0
		for _, n := range lacing {
			size += int(n)
		}
		body := make([]byte, size)
		if _, err := io.ReadFull(r, body); err != nil {
			return nil, ""
		}
		pageSerial := binary.LittleEndian.Uint32(header[14:18])
		pageSequence := binary.LittleEndian.Uint32(header[18:22])
		if !selected {
			if header[5]&2 == 0 {
				return nil, ""
			}
			serial, sequence, selected = pageSerial, pageSequence, true
		} else if pageSerial != serial {
			continue
		} else {
			if pageSequence != sequence+1 {
				return nil, ""
			}
			sequence = pageSequence
		}
		if (header[5]&1 != 0) != (len(packet) != 0) {
			return nil, ""
		}
		for _, n := range lacing {
			if len(packet)+int(n) > maxCoverMetadataBytes {
				return nil, ""
			}
			packet = append(packet, body[:int(n)]...)
			body = body[int(n):]
			if n == 255 {
				continue
			}
			if !identified {
				opus = bytes.HasPrefix(packet, []byte("OpusHead"))
				if !opus && !bytes.HasPrefix(packet, []byte("\x01vorbis")) {
					return nil, ""
				}
				identified = true
				packet = nil
				continue
			}
			prefix := []byte("\x03vorbis")
			if opus {
				prefix = []byte("OpusTags")
			}
			if !bytes.HasPrefix(packet, prefix) {
				return nil, ""
			}
			return frontCoverComments(packet[len(prefix):])
		}
	}
	return nil, ""
}

func frontCoverComments(payload []byte) ([]byte, string) {
	if _, ok := coverLengthField(&payload, binary.LittleEndian); !ok || len(payload) < 4 {
		return nil, ""
	}
	count := binary.LittleEndian.Uint32(payload[:4])
	payload = payload[4:]
	for i := uint32(0); i < count; i++ {
		field, ok := coverLengthField(&payload, binary.LittleEndian)
		if !ok {
			return nil, ""
		}
		key, value, found := bytes.Cut(field, []byte("="))
		if !found || !strings.EqualFold(string(key), "METADATA_BLOCK_PICTURE") || len(value) > base64.StdEncoding.EncodedLen(maxCoverBytes+65536) {
			continue
		}
		block, err := base64.StdEncoding.DecodeString(string(value))
		if err != nil {
			continue
		}
		if data, mime := frontPictureBlock(block); data != nil {
			return data, mime
		}
	}
	return nil, ""
}
