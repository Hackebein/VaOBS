package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/andreykaipov/goobs"
	"github.com/andreykaipov/goobs/api/requests/inputs"
	"github.com/fsnotify/fsnotify"
)

var (
	urlRegex     = regexp.MustCompile(`\[Video Playback\] (?:URL.*resolved to '|Resolving URL ')([^']+)'`)
	obsClient    *goobs.Client
	obsConnected = false

	// Command line flags
	obsHost            = flag.String("obs-host", "localhost", "OBS WebSocket host")
	obsPort            = flag.Int("obs-port", 4455, "OBS WebSocket port")
	obsPassword        = flag.String("obs-password", "", "OBS WebSocket password")
	inputName          = flag.String("input-name", "VRChatFeed", "OBS input source name")
	additionalSettings = flag.String("additional-settings", "hw_decode=true,close_when_inactive=true", "Additional input settings in format key1=value1,key2=value2")
)

func getLatestLogFile() (string, error) {
	homeDir, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}

	logDir := filepath.Join(homeDir, "AppData", "LocalLow", "VRChat", "VRChat")
	pattern := filepath.Join(logDir, "output_log_*.txt")

	files, err := filepath.Glob(pattern)
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

func parseAdditionalSettings() map[string]interface{} {
	settings := make(map[string]interface{})

	if *additionalSettings == "" {
		return settings
	}

	pairs := strings.Split(*additionalSettings, ",")
	for _, pair := range pairs {
		parts := strings.SplitN(pair, "=", 2)
		if len(parts) != 2 {
			log.Printf("Invalid setting format: %s (expected key=value)", pair)
			continue
		}

		key := strings.TrimSpace(parts[0])
		value := strings.TrimSpace(parts[1])

		// Try to parse as different types
		if parsedBool, err := strconv.ParseBool(value); err == nil {
			settings[key] = parsedBool
		} else if parsedInt, err := strconv.ParseInt(value, 10, 64); err == nil {
			settings[key] = parsedInt
		} else if parsedFloat, err := strconv.ParseFloat(value, 64); err == nil {
			settings[key] = parsedFloat
		} else {
			// Store as string
			settings[key] = value
		}
	}

	return settings
}

func pushToOBS(url string) {
	// Convert rtspt protocol to rtmp for streams
	if strings.HasPrefix(url, "rtspt://") {
		url = strings.Replace(url, "rtspt://", "rtmp://", 1)
		log.Printf("Converted rtspt to rtmp: %s", url)
	}

	if obsConnected {
		// Create input settings
		inputSettings := map[string]interface{}{}

		// Add additional settings
		additionalSettings := parseAdditionalSettings()
		for key, value := range additionalSettings {
			inputSettings[key] = value
		}

		// Set the URL
		inputSettings["input"] = url
		inputSettings["is_local_file"] = false

		// Update the input settings
		overlay := false
		_, err := obsClient.Inputs.SetInputSettings(&inputs.SetInputSettingsParams{
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
		if matches := urlRegex.FindStringSubmatch(line); matches != nil && len(matches) > 1 {
			lastURL = matches[1]
		}
	}

	if err := scanner.Err(); err != nil {
		log.Printf("Error reading log file: %v", err)
		return ""
	}

	return lastURL
}

func monitorLogFile(filename string, watcher *fsnotify.Watcher) {
	// Find and push the last URL from the existing log
	lastURL := findLastURLInFile(filename)
	if lastURL != "" {
		log.Printf("Found last URL in log: %s", lastURL)
		pushToOBS(lastURL)
	} else {
		log.Println("No previous URLs found in log")
	}

	// Keep track of file position
	file, err := os.Open(filename)
	if err != nil {
		log.Printf("Error opening log file: %v", err)
		return
	}
	defer file.Close()

	// Seek to end of file to only read new content
	file.Seek(0, 2)

	for {
		select {
		case event, ok := <-watcher.Events:
			if !ok {
				return
			}

			if event.Op&fsnotify.Write == fsnotify.Write {
				// Read new content from file
				scanner := bufio.NewScanner(file)
				for scanner.Scan() {
					line := scanner.Text()
					if matches := urlRegex.FindStringSubmatch(line); matches != nil && len(matches) > 1 {
						url := matches[1]
						pushToOBS(url)
					}
				}
			}

		case err, ok := <-watcher.Errors:
			if !ok {
				return
			}
			log.Printf("Watcher error: %v", err)
		}
	}
}

func main() {
	// Parse command line flags
	flag.Parse()

	log.Printf("Starting VRChat URL to OBS monitor with settings:")
	log.Printf("  OBS Host: %s", *obsHost)
	log.Printf("  OBS Port: %d", *obsPort)
	log.Printf("  Input Name: %s", *inputName)
	if *additionalSettings != "" {
		log.Printf("  Additional Settings: %s", *additionalSettings)
	}

	// Connect to OBS
	connectToOBS()
	defer func() {
		if obsConnected {
			obsClient.Disconnect()
		}
	}()

	// Get initial log file
	currentLogFile, err := getLatestLogFile()
	if err != nil {
		log.Fatalf("Error finding log file: %v", err)
	}

	log.Printf("Monitoring VRChat log file: %s", currentLogFile)

	// Create file watcher
	watcher, err := fsnotify.NewWatcher()
	if err != nil {
		log.Fatalf("Error creating file watcher: %v", err)
	}
	defer watcher.Close()

	// Add the log file to watcher
	err = watcher.Add(currentLogFile)
	if err != nil {
		log.Fatalf("Error adding file to watcher: %v", err)
	}

	// Start monitoring in a goroutine
	go monitorLogFile(currentLogFile, watcher)

	// Check for new log files periodically
	checkInterval := 5 * time.Second
	ticker := time.NewTicker(checkInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			latestLog, err := getLatestLogFile()
			if err != nil {
				log.Printf("Error checking for new log files: %v", err)
				continue
			}

			if latestLog != currentLogFile {
				log.Printf("New log file detected: %s", latestLog)
				log.Println("Switching to monitor new log file...")

				// Remove old file from watcher
				watcher.Remove(currentLogFile)

				// Add new file to watcher
				err = watcher.Add(latestLog)
				if err != nil {
					log.Printf("Error adding new file to watcher: %v", err)
					continue
				}

				currentLogFile = latestLog
				log.Printf("Now monitoring: %s", currentLogFile)

				// Start monitoring new file
				go monitorLogFile(currentLogFile, watcher)
			}
		}
	}
}
