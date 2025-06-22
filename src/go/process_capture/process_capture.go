package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"

	_ "golang.org/x/image/tiff"

	"github.com/fogleman/gg"
)

type MetaInfo struct {
	Timestamp        float64 `json:"Timestamp"`
	FrameNumber      int     `json:"FrameNumber"`
	RelativeTime     float64 `json:"RelativeTime"`
	MaxPixelValue    float64 `json:"MaxPixelValue"`
	MinPixelValue    float64 `json:"MinPixelValue"`
	MeanPixelValue   float64 `json:"MeanPixelValue"`
	MedianPixelValue float64 `json:"MedianPixelValue"`
	Shape            []int   `json:"shape"`
}

func extractMetadata(desc string) (*MetaInfo, error) {
	var meta MetaInfo
	err := json.Unmarshal([]byte(desc), &meta)
	return &meta, err
}

func demosaicBGGR(gray *image.Gray) *image.RGBA {
	bounds := gray.Bounds()
	rgba := image.NewRGBA(bounds)
	for y := 0; y < bounds.Dy()-1; y++ {
		for x := 0; x < bounds.Dx()-1; x++ {
			var r, g, b uint8
			p := gray.GrayAt(x, y).Y
			if y%2 == 0 {
				if x%2 == 0 {
					// B
					b = p
					g = (gray.GrayAt(x+1, y).Y + gray.GrayAt(x, y+1).Y) / 2
					r = gray.GrayAt(x+1, y+1).Y
				} else {
					// G (on blue row)
					g = p
					b = (gray.GrayAt(x-1, y).Y + gray.GrayAt(x+1, y).Y) / 2
					r = (gray.GrayAt(x, y+1).Y + gray.GrayAt(x+1, y+1).Y) / 2
				}
			} else {
				if x%2 == 0 {
					// G (on red row)
					g = p
					b = (gray.GrayAt(x, y-1).Y + gray.GrayAt(x+1, y-1).Y) / 2
					r = (gray.GrayAt(x-1, y).Y + gray.GrayAt(x+1, y).Y) / 2
				} else {
					// R
					r = p
					g = (gray.GrayAt(x-1, y).Y + gray.GrayAt(x, y-1).Y) / 2
					b = gray.GrayAt(x-1, y-1).Y
				}
			}
			rgba.Set(x, y, color.RGBA{r, g, b, 255})
		}
	}
	return rgba
}

func drawOverlay(img image.Image, meta *MetaInfo, index int, total int) image.Image {
	dc := gg.NewContextForImage(img)
	dc.SetRGBA(0, 0, 0, 0.7)
	dc.DrawRectangle(0, float64(dc.Height()-40), float64(dc.Width()), 40)
	dc.Fill()
	dc.SetRGB(1, 1, 1)
	dc.DrawStringAnchored(
		fmt.Sprintf("Frame %d/%d, Time %.3f s", meta.FrameNumber, total, meta.RelativeTime),
		float64(dc.Width()/2), float64(dc.Height()-20), 0.5, 0.5,
	)
	return dc.Image()
}

func processTIFFs(inputDir string, outputDir string) error {
	files, err := filepath.Glob(filepath.Join(inputDir, "*.tiff"))
	if err != nil {
		return err
	}
	sort.Strings(files)
	total := len(files)

	err = os.MkdirAll(outputDir, 0755)
	if err != nil {
		return err
	}

	for i, file := range files {
		f, err := os.Open(file)
		if err != nil {
			return err
		}
		img, format, err := image.Decode(f)
		f.Close()
		if err != nil {
			return err
		}
		if format != "tiff" {
			continue
		}

		// získej metadata (ImageDescription)
		f, _ = os.Open(file)
		buf := new(bytes.Buffer)
		buf.ReadFrom(f)
		f.Close()
		raw := buf.String()
		idx := strings.Index(raw, "ImageDescription")
		if idx == -1 {
			continue
		}
		metaStart := strings.Index(raw[idx:], "{")
		metaEnd := strings.Index(raw[idx:], "}") + 1
		jsonMeta := raw[idx+metaStart : idx+metaEnd]
		meta, err := extractMetadata(jsonMeta)
		if err != nil {
			log.Printf("JSON parse error in %s: %v", file, err)
			continue
		}

		// Bayer demosaic
		grayImg, ok := img.(*image.Gray)
		if !ok {
			log.Printf("Image is not grayscale: %s", file)
			continue
		}
		colorImg := demosaicBGGR(grayImg)
		finalImg := drawOverlay(colorImg, meta, i+1, total)

		outfile := filepath.Join(outputDir, fmt.Sprintf("frame_%04d.jpg", i+1))
		outf, err := os.Create(outfile)
		if err != nil {
			return err
		}
		err = jpeg.Encode(outf, finalImg, &jpeg.Options{Quality: 95})
		outf.Close()
		if err != nil {
			return err
		}
		log.Printf("Wrote %s", outfile)
	}

	// vytvoř MP4 pomocí ffmpeg
	cmd := exec.Command("ffmpeg", "-y", "-framerate", "30", "-i", filepath.Join(outputDir, "frame_%04d.jpg"), "-c:v", "libx264", "-pix_fmt", "yuv420p", "output.mp4")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func main() {
	inputDir := "./"       // aktuální složka
	outputDir := "preview" // náhledy

	err := processTIFFs(inputDir, outputDir)
	if err != nil {
		log.Fatal(err)
	}
}
