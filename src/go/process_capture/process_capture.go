package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"image"
	"image/jpeg"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/fogleman/gg"
	"gocv.io/x/gocv"
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

func processTIFFs(inputDir string, outputFile string) error {
	files, err := filepath.Glob(filepath.Join(inputDir, "*.tiff"))
	if err != nil {
		return err
	}
	sort.Strings(files)
	total := len(files)

	// Get first image to determine video dimensions
	firstImg := gocv.IMRead(files[0], gocv.IMReadGrayScale)
	if firstImg.Empty() {
		return fmt.Errorf("Failed to read first image for dimensions")
	}
	width := firstImg.Cols()
	height := firstImg.Rows()
	firstImg.Close()

	// Create VideoWriter
	writer, err := gocv.VideoWriterFile(outputFile, "avc1", 30.0, width, height, true)
	if err != nil {
		return fmt.Errorf("Error creating video writer: %v", err)
	}
	defer writer.Close()

	for i, file := range files {
		// Load image with GoCV
		img := gocv.IMRead(file, gocv.IMReadGrayScale)
		if img.Empty() {
			log.Printf("Failed to read image: %s", file)
			continue
		}

		// Extract metadata
		f, _ := os.Open(file)
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

		// Debayer using OpenCV
		colorImg := gocv.NewMat()
		gocv.CvtColor(img, &colorImg, gocv.ColorBayerBG2RGB)

		// Convert to Go image for overlay
		goImg, _ := colorImg.ToImage()
		finalImg := drawOverlay(goImg, meta, i+1, total)

		// Convert back to Mat for video writing
		buf = new(bytes.Buffer)
		jpeg.Encode(buf, finalImg, nil)
		data := buf.Bytes()
		mat, err := gocv.IMDecode(data, gocv.IMReadColor)
		if err != nil {
			log.Printf("Error decoding image: %v", err)
			continue
		}

		// Write frame to video
		writer.Write(mat)
		log.Printf("Processed frame %d/%d", i+1, total)

		// Free resources
		img.Close()
		colorImg.Close()
		mat.Close()
	}

	return nil
}

func main() {
	inputDir := "./"           // aktuální složka
	outputFile := "output.mp4" // výstupní video

	err := processTIFFs(inputDir, outputFile)
	if err != nil {
		log.Fatal(err)
	}
}
