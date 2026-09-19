package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

func coverPNG(t *testing.T, c color.RGBA) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, 8, 8))
	for y := 0; y < 8; y++ {
		for x := 0; x < 8; x++ {
			img.SetRGBA(x, y, c)
		}
	}
	var out bytes.Buffer
	if err := png.Encode(&out, img); err != nil {
		t.Fatal(err)
	}
	return out.Bytes()
}
func coverWrite(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
}
func coverFFmpeg(t *testing.T, args ...string) {
	t.Helper()
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg is required to generate real audio fixtures")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "ffmpeg", append([]string{"-v", "error", "-nostdin", "-y"}, args...)...)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("ffmpeg: %v: %s", err, out)
	}
}
func pictureBlock(role uint32, data []byte) []byte {
	var out bytes.Buffer
	write := func(n uint32) { _ = binary.Write(&out, binary.BigEndian, n) }
	write(role)
	write(9)
	out.WriteString("image/png")
	write(0)
	write(8)
	write(8)
	write(24)
	write(0)
	write(uint32(len(data)))
	out.Write(data)
	return out.Bytes()
}
func coverComments(blocks ...[]byte) []byte {
	var out bytes.Buffer
	write := func(n uint32) { _ = binary.Write(&out, binary.LittleEndian, n) }
	write(0)
	write(uint32(len(blocks)))
	for _, block := range blocks {
		field := "METADATA_BLOCK_PICTURE=" + base64.StdEncoding.EncodeToString(block)
		write(uint32(len(field)))
		out.WriteString(field)
	}
	return out.Bytes()
}
func oggCoverCRC(page []byte) {
	clear(page[22:26])
	var crc uint32
	for _, b := range page {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = crc<<1 ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	binary.LittleEndian.PutUint32(page[22:26], crc)
}

// Replace the comment packet in a real ffmpeg-created Vorbis/Opus file,
// preserving the setup/audio packets, stream serial, page sequence and CRC.
func replaceOggCoverComments(t *testing.T, source []byte, comments []byte, opus bool) []byte {
	t.Helper()
	prefix := []byte("\x03vorbis")
	if opus {
		prefix = []byte("OpusTags")
	}
	var output []byte
	replaced := false
	for len(source) > 0 {
		if len(source) < 27 {
			t.Fatal("short Ogg page")
		}
		headerSize := 27 + int(source[26])
		size := 0
		for _, n := range source[27:headerSize] {
			size += int(n)
		}
		page := source[:headerSize+size]
		body := page[headerSize:]
		if !replaced && bytes.HasPrefix(body, prefix) {
			packetSize, segments := 0, 0
			for _, n := range source[27:headerSize] {
				packetSize += int(n)
				segments++
				if n < 255 {
					break
				}
			}
			packet := append(append([]byte{}, prefix...), comments...)
			if !opus {
				packet = append(packet, 1)
			} // Vorbis comment framing bit
			lace := make([]byte, len(packet)/255+1)
			for i := range lace {
				lace[i] = 255
			}
			lace[len(lace)-1] = byte(len(packet) % 255)
			lace = append(lace, source[27+segments:headerSize]...)
			if len(lace) > 255 {
				t.Fatal("test packet too large")
			}
			newPage := append([]byte{}, page[:27]...)
			newPage[26] = byte(len(lace))
			newPage = append(newPage, lace...)
			newPage = append(newPage, packet...)
			newPage = append(newPage, body[packetSize:]...)
			oggCoverCRC(newPage)
			output = append(output, newPage...)
			replaced = true
		} else {
			output = append(output, page...)
		}
		source = source[len(page):]
	}
	if !replaced {
		t.Fatal("comment packet not found")
	}
	return output
}

func TestCoverArtFrontOnlyRealFiles(t *testing.T) {
	front := coverPNG(t, color.RGBA{R: 255, A: 255})
	artist := coverPNG(t, color.RGBA{B: 255, A: 255})
	for _, format := range []string{"flac", "mp3", "ogg", "opus", "m4a"} {
		t.Run(format, func(t *testing.T) {
			dir := t.TempDir()
			plain := filepath.Join(dir, "plain."+format)
			coverFFmpeg(t, "-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono", "-t", "0.05", plain)
			coverWrite(t, filepath.Join(dir, "front.png"), front)
			coverWrite(t, filepath.Join(dir, "artist.png"), artist)
			if format == "m4a" {
				target := filepath.Join(dir, "covered.m4a")
				coverFFmpeg(t, "-i", plain, "-i", filepath.Join(dir, "front.png"), "-map", "0:a", "-map", "1:v", "-c", "copy", "-disposition:v:0", "attached_pic", target)
				data, mime := extractCoverArt(target)
				if !bytes.Equal(data, front) || mime != "image/png" {
					t.Fatal("MP4 covr cover was lost")
				}
				return
			}
			for _, roles := range [][]uint32{{8, 3}, {3, 8}, {8}, {4}, {0}, {3, 3}} {
				t.Run(fmt.Sprint(roles), func(t *testing.T) {
					target := filepath.Join(dir, "mixed."+format)
					if format == "ogg" || format == "opus" {
						source, err := os.ReadFile(plain)
						if err != nil {
							t.Fatal(err)
						}
						var blocks [][]byte
						for _, role := range roles {
							pic := artist
							if role == 3 {
								pic = front
							}
							blocks = append(blocks, pictureBlock(role, pic))
						}
						coverWrite(t, target, replaceOggCoverComments(t, source, coverComments(blocks...), format == "opus"))
					} else {
						args := []string{"-i", plain}
						for _, role := range roles {
							name := "artist.png"
							if role == 3 {
								name = "front.png"
							}
							args = append(args, "-i", filepath.Join(dir, name))
						}
						args = append(args, "-map", "0:a")
						for i := range roles {
							args = append(args, "-map", strconv.Itoa(i+1)+":v")
						}
						args = append(args, "-c", "copy")
						names := map[uint32]string{0: "Other", 3: "Cover (front)", 4: "Cover (back)", 8: "Artist/performer"}
						for i, role := range roles {
							args = append(args, "-disposition:v:"+strconv.Itoa(i), "attached_pic", "-metadata:s:v:"+strconv.Itoa(i), "comment="+names[role], "-metadata:s:v:"+strconv.Itoa(i), "title=Picture "+strconv.Itoa(i))
						}
						args = append(args, target)
						coverFFmpeg(t, args...)
					}
					data, mime := extractCoverArt(target)
					wantFront := false
					for _, role := range roles {
						wantFront = wantFront || role == 3
					}
					if wantFront {
						if !bytes.Equal(data, front) || mime != "image/png" {
							t.Fatal("did not select front cover")
						}
					} else {
						if data != nil {
							t.Fatal("artist/back/other image accepted as cover")
						}
						fallback, kind := getCoverArt(target)
						if !bytes.Equal(fallback, front) || kind != "image/png" {
							t.Fatal("missing front did not fall back to folder front.png")
						}
					}
					a := &app{}
					a.cfg.Library.MusicDir = dir
					album := dispatchCapture(t, a, "albumart "+filepath.Base(target)+" 0")
					binaryFront := "binary: " + strconv.Itoa(len(front)) + "\n" + string(front) + "\n"
					if !strings.Contains(album, binaryFront) {
						t.Fatal("MPD albumart did not serve the front cover")
					}
					embedded := dispatchCapture(t, a, "readpicture "+filepath.Base(target)+" 0")
					if wantFront {
						if !strings.Contains(embedded, binaryFront) {
							t.Fatal("MPD readpicture did not serve the front cover")
						}
						offset := len(front) / 2
						rest := dispatchCapture(t, a, "readpicture "+filepath.Base(target)+" "+strconv.Itoa(offset))
						if !strings.Contains(rest, "binary: "+strconv.Itoa(len(front)-offset)+"\n"+string(front[offset:])+"\n") {
							t.Fatal("MPD artwork offset changed the selected picture")
						}
					} else if !strings.Contains(embedded, "size: 0\n") || strings.Contains(embedded, "binary:") {
						t.Fatal("MPD readpicture returned a non-front picture")
					}

				})
			}
		})
	}
}

func TestCoverArtNoArtistFolderFallback(t *testing.T) {
	dir := t.TempDir()
	track := filepath.Join(dir, "plain.flac")
	coverFFmpeg(t, "-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono", "-t", "0.05", track)
	coverWrite(t, filepath.Join(dir, "artist.png"), coverPNG(t, color.RGBA{B: 255, A: 255}))
	if data, _ := getCoverArt(track); data != nil {
		t.Fatal("artist.png must not be used as cover art")
	}
}

func TestCoverArtMalformedPictures(t *testing.T) {
	for _, payload := range [][]byte{nil, {0, 0, 0, 3}, {0, 0, 0, 3, 255, 255, 255, 255}, pictureBlock(8, []byte("not front"))} {
		if data, _ := frontPictureBlock(payload); data != nil {
			t.Fatal("invalid/non-front picture accepted")
		}
	}
	for _, payload := range [][]byte{nil, {255, 255, 255, 255}, {0, 0, 0, 0, 255, 255, 255, 255}} {
		if data, _ := frontCoverComments(payload); data != nil {
			t.Fatal("truncated comments accepted")
		}
	}
	if data, _ := readFLACFrontCover(bytes.NewReader([]byte{0x86, 0xff, 0xff, 0xff})); data != nil {
		t.Fatal("truncated FLAC accepted")
	}
	if data, _ := readOggFrontCover(bytes.NewReader([]byte("OggS"))); data != nil {
		t.Fatal("truncated Ogg accepted")
	}
}

func TestCoverArtOggContinuedCommentPacket(t *testing.T) {
	front := coverPNG(t, color.RGBA{R: 255, A: 255})
	comments := coverComments(pictureBlock(8, []byte("artist")), pictureBlock(3, front))
	// A large vendor field forces the comment packet across Ogg page boundaries.
	var payload bytes.Buffer
	payload.WriteString("OpusTags")
	_ = binary.Write(&payload, binary.LittleEndian, uint32(120000))
	payload.Write(make([]byte, 120000))
	payload.Write(comments[4:])
	var pages [][]byte
	addPacket := func(packet []byte, beginning bool) {
		continued := false
		for {
			n := min(len(packet), 65025)
			lace := make([]byte, min(n/255+1, 255))
			for i := range lace {
				lace[i] = 255
			}
			if n < 65025 {
				lace[len(lace)-1] = byte(n % 255)
			}
			page := make([]byte, 27)
			copy(page, "OggS")
			if beginning {
				page[5] = 2
			}
			if continued {
				page[5] |= 1
			}
			binary.LittleEndian.PutUint32(page[14:18], 1)
			binary.LittleEndian.PutUint32(page[18:22], uint32(len(pages)))
			page[26] = byte(len(lace))
			page = append(page, lace...)
			page = append(page, packet[:n]...)
			oggCoverCRC(page)
			pages = append(pages, page)
			packet = packet[n:]
			if n < 65025 {
				break
			}
			beginning = false
			continued = true
		}
	}
	addPacket([]byte("OpusHead"), true)
	addPacket(payload.Bytes(), false)
	data, mime := readOggFrontCover(bytes.NewReader(bytes.Join(pages, nil)))
	if !bytes.Equal(data, front) || mime != "image/png" {
		t.Fatal("front cover in continued comment packet was lost")
	}
	pages[2][5] &^= 1
	if data, _ := readOggFrontCover(bytes.NewReader(bytes.Join(pages, nil))); data != nil {
		t.Fatal("broken packet continuation accepted")
	}
}
