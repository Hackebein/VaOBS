package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"
	"time"

	"github.com/andreykaipov/goobs"
	"github.com/andreykaipov/goobs/api/requests/inputs"
	"github.com/fsnotify/fsnotify"
)

var (
	urlRegex     = regexp.MustCompile(`\[Video Playback\] (?:URL.*resolved to '|Resolving URL ')([^']+)'`)
	quitRegex    = regexp.MustCompile(`VRCApplication: HandleApplicationQuit|\[Behaviour\] Successfully left room`)
	obsClient    *goobs.Client
	obsConnected = false

	// Command line flags
	obsHost          = flag.String("obs-host", "localhost", "OBS WebSocket host")
	obsPort          = flag.Int("obs-port", 4455, "OBS WebSocket port")
	obsPassword      = flag.String("obs-password", "", "OBS WebSocket password")
	inputName        = flag.String("input-name", "VRChatFeed", "OBS input source name")
	rtsptReplacement = flag.String("rtspt-replacement", "rtmp", "Protocol to replace rtspt with")
)

func getVRChatLogDir() (string, error) {
	homeDir, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}

	return filepath.Join(homeDir, "AppData", "LocalLow", "VRChat", "VRChat"), nil
}

func getLatestLogFile() (string, error) {
	logDir, err := getVRChatLogDir()
	if err != nil {
		return "", err
	}

	files, err := filepath.Glob(filepath.Join(logDir, "output_log_*.txt"))
	if err != nil {
		return "", err
	}

	if len(files) == 0 {
		return "", fmt.Errorf("no VRChat log files found")
	}

	// Find the most recent file
	var latestFile string
	var latestTime time.Time

	for _, file := range files {
		info, err := os.Stat(file)
		if err != nil {
			continue
		}

		if info.ModTime().After(latestTime) {
			latestTime = info.ModTime()
			latestFile = file
		}
	}

	if latestFile == "" {
		return "", fmt.Errorf("unable to find VRChat log files")
	}

	return latestFile, nil
}

func connectToOBS() {
	var err error
	obsAddress := fmt.Sprintf("%s:%d", *obsHost, *obsPort)
	obsClient, err = goobs.New(obsAddress, goobs.WithPassword(*obsPassword))
	if err != nil {
		log.Printf("Failed to connect to OBS WebSocket: %v", err)
		log.Println("The program will continue monitoring VRChat logs, but won't update OBS.")
		log.Println("Make sure OBS Studio is running and WebSocket server is enabled.")
		return
	}

	obsConnected = true
	log.Printf("Connected to OBS WebSocket successfully at %s!", obsAddress)
}

func pushToOBS(url string) {
	// Convert rtspt protocol
	if strings.HasPrefix(url, "rtspt://") {
		url = strings.Replace(url, "rtspt://", *rtsptReplacement+"://", 1)
		log.Printf("Converted rtspt to %s: %s", *rtsptReplacement, url)
	}

	if obsConnected {
		// Get current input settings first
		response, err := obsClient.Inputs.GetInputSettings(&inputs.GetInputSettingsParams{
			InputName: inputName,
		})

		if err != nil {
			log.Printf("Failed to get OBS input settings: %v", err)
			return
		}
		inputSettings := response.InputSettings
		inputSettings["input"] = url
		inputSettings["is_local_file"] = false
		inputSettings["seekable"] = false

		// Update the input settings
		overlay := false
		_, err = obsClient.Inputs.SetInputSettings(&inputs.SetInputSettingsParams{
			InputName:     inputName,
			InputSettings: inputSettings,
			Overlay:       &overlay,
		})

		if err != nil {
			log.Printf("Failed to update OBS: %v", err)
		} else {
			log.Printf("Updated OBS input '%s' with URL: %s", *inputName, url)
		}
	} else {
		log.Printf("OBS not connected. URL detected: %s", url)
	}
}

func findLastURLInFile(filename string) string {
	file, err := os.Open(filename)
	if err != nil {
		log.Printf("Error opening log file: %v", err)
		return ""
	}
	defer file.Close()

	var lastURL string
	scanner := bufio.NewScanner(file)

	for scanner.Scan() {
		line := scanner.Text()
		if matches := urlRegex.FindStringSubmatch(line); len(matches) > 1 {
			lastURL = matches[1]
		}
		if quitRegex.MatchString(line) {
			lastURL = "" // Clear any previous URL
		}
	}

	if err := scanner.Err(); err != nil {
		log.Printf("Error reading log file: %v", err)
		return ""
	}

	return lastURL
}

func monitorLogFile(filename string, stopChan chan struct{}) {
	log.Printf("Gopher spawned: Monitoring content of %s", filepath.Base(filename))

	// Find and push the last URL from the existing log
	lastURL := findLastURLInFile(filename)
	pushToOBS(lastURL)

	// Establish initial file bounds
	info, err := os.Stat(filename)
	if err != nil {
		log.Printf("Error reading log file: %v", err)
		return
	}
	lastSize := info.Size()

	ticker := time.NewTicker(500 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-stopChan:
			log.Printf("Stopping monitor goroutine for: %s", filepath.Base(filename))
			return
		case <-ticker.C:
			fi, err := os.Stat(filename)
			if err != nil {
				continue
			}

			currentSize := fi.Size()
			if currentSize < lastSize {
				// File was truncated or reset
				lastSize = 0
			}

			if currentSize > lastSize {
				err := readAndProcessNewContent(filename, lastSize, currentSize)
				if err == nil {
					lastSize = currentSize
				} else {
					log.Printf("Error processing updates: %v", err)
				}
			}
		}
	}
}

func readAndProcessNewContent(filename string, offset, currentSize int64) error {
	file, err := os.OpenFile(filename, os.O_RDONLY, 0)
	if err != nil {
		return err
	}
	defer file.Close()

	_, err = file.Seek(offset, io.SeekStart)
	if err != nil {
		return err
	}

	// Only read exactly what was added since our last check
	scanner := bufio.NewScanner(io.LimitReader(file, currentSize-offset))
	for scanner.Scan() {
		line := scanner.Text()
		if matches := urlRegex.FindStringSubmatch(line); len(matches) > 1 {
			url := matches[1]
			pushToOBS(url)
		} else if quitRegex.MatchString(line) {
			pushToOBS("") // Clear OBS input on quit/leave
		}
	}
	return scanner.Err()
}

func main() {
	// Parse command line flags
	flag.Parse()

	log.Printf("Starting VRChat URL to OBS monitor with settings:")
	log.Printf("  OBS Host: %s", *obsHost)
	log.Printf("  OBS Port: %d", *obsPort)
	log.Printf("  Input Name: %s", *inputName)
	log.Printf("  Replace rtspt with: %v", *rtsptReplacement)

	// Connect to OBS
	connectToOBS()
	defer func() {
		if obsConnected {
			// Clear OBS input before disconnecting
			pushToOBS("")
			obsClient.Disconnect()
		}
	}()

	// Setup signal handling for graceful shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	logDir, err := getVRChatLogDir()
	if err != nil {
		log.Fatalf("Error finding VRChat log directory: %v", err)
	}

	watcher, err := fsnotify.NewWatcher()
	if err != nil {
		log.Fatalf("Error creating file watcher: %v", err)
	}
	defer watcher.Close()

	if err := watcher.Add(logDir); err != nil {
		log.Fatalf("Error watching VRChat log directory: %v", err)
	}

	// Get initial log file
	currentLogFile, err := getLatestLogFile()
	if err != nil {
		log.Fatalf("Error finding log file: %v", err)
	}

	log.Printf("Monitoring VRChat log file: %s", currentLogFile)

	// Start monitoring in a goroutine with a stop channel
	stopChan := make(chan struct{})
	go monitorLogFile(currentLogFile, stopChan)

	// Handle graceful shutdown in a goroutine
	go func() {
		<-sigChan
		log.Println("Received shutdown signal, clearing OBS input...")
		close(stopChan)
		if obsConnected {
			pushToOBS("")
		}
		os.Exit(0)
	}()

	for {
		select {
		case <-sigChan:
			log.Println("Received shutdown signal, clearing OBS input...")
			close(stopChan)
			pushToOBS("")
			return

		case event, ok := <-watcher.Events:
			if !ok {
				return
			}

			baseName := filepath.Base(event.Name)
			isLogFile, _ := filepath.Match("output_log_*.txt", baseName)
			if !isLogFile {
				continue
			}

			if event.Op&(fsnotify.Write) == 0 {
				continue
			}

			latestLog, err := getLatestLogFile()
			if err != nil {
				log.Printf("Error checking latest log file: %v", err)
				continue
			}

			if latestLog == currentLogFile {
				continue
			}

			log.Printf("New latest log file detected: %s", latestLog)

			close(stopChan)
			stopChan = make(chan struct{})
			currentLogFile = latestLog

			go monitorLogFile(currentLogFile, stopChan)

		case err, ok := <-watcher.Errors:
			if ok {
				log.Printf("File watcher error: %v", err)
			}
		}
	}
}
