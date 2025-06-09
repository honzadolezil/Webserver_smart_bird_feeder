# Gallery backend for my ESP32-CAM system

#libraries
from flask import Flask, request, send_from_directory, render_template, jsonify
import requests
import os
import json
from datetime import datetime

app = Flask(__name__)

# config - where to store and parameters..
IMAGES_FOLDER = "captured_images"
WEATHER_DATA_FILE = "weather_data.json"
MAX_IMAGES = 10000  
AUTOCLEANUP = True # cleanup old images if too many
MAX_WEATHER_RECORDS = 100000000 # max weather records so json isnt too big 
ESP32_IP = "192.168.244.101"

# caching list not to reload from file every time
weather_data = []
latest_weather = None

# create the directories if dont exist
for folder in [IMAGES_FOLDER, 'templates', 'static']:
    if not os.path.exists(folder):
        os.makedirs(folder)

def load_weather_data():
    """load weather data from jsno file - on startup"""
    global weather_data, latest_weather
    
    # if weather data file doesnt exist create empty list
    if not os.path.exists(WEATHER_DATA_FILE):
        weather_data = []
        return
    
    try:
        with open(WEATHER_DATA_FILE, 'r') as f:
            weather_data = json.load(f)
        
        if weather_data:
            latest_weather = weather_data[-1] # to display latest weather data
            
    except Exception as e: 
        #error loading weather data
        weather_data = []

def save_weather_data():
    """savin weather data to json file"""
    try:
        # Keep only recent records
        if len(weather_data) > MAX_WEATHER_RECORDS:
            weather_data[:] = weather_data[-MAX_WEATHER_RECORDS:]
        
        with open(WEATHER_DATA_FILE, 'w') as f:
            json.dump(weather_data, f)
        print(f"Saved {len(weather_data)} weather records")
    except Exception as e:
        print(f"Error saving weather data: {e}")

def add_weather_record(temperature, humidity, pressure):
    """adds new weather record"""
    global latest_weather
    
    record = {
        'timestamp': datetime.now().isoformat(), # nevermind adding time after measurement - we can use current time
        'temperature': temperature,
        'humidity': humidity,
        'pressure': pressure
    }
    
    weather_data.append(record)
    latest_weather = record
    save_weather_data()
    return record

def get_image_list():
    """listing of all images"""
    if not os.path.exists(IMAGES_FOLDER):
        return []
    
    images = []
    for filename in os.listdir(IMAGES_FOLDER):
        if filename.endswith('.jpg'):
            filepath = os.path.join(IMAGES_FOLDER, filename)
            # Extract timestamp from filename
            try:
                timestamp_str = filename.replace('.jpg', '')
                timestamp = datetime.strptime(timestamp_str, '%Y%m%d_%H%M%S') #used format from esp32cam
                formatted_time = timestamp.strftime('%Y-%m-%d %H:%M:%S')
            except:
                formatted_time = "Unknown" # wrong filename format
            
            images.append({
                'filename': filename,
                'timestamp': formatted_time,
                'mtime': os.path.getmtime(filepath) # mod time - to track when image was saved to drive
            })
    
    # mtime sorting newest first
    images.sort(key=lambda x: x['mtime'], reverse=True)
    return images

def get_latest_image(): # qucik extraction of the lates image of list
    images = get_image_list()
    return images[0] if images else None

def cleanup_old_images():
    """delete oldest images if too many of them
    user should copy them to another location before"""
    if not os.path.exists(IMAGES_FOLDER):
        return
        
    image_files = [os.path.join(IMAGES_FOLDER, f) for f in os.listdir(IMAGES_FOLDER)  # get all files in folder
                  if f.endswith('.jpg')]
    
    if len(image_files) > MAX_IMAGES:
        image_files.sort(key=os.path.getmtime)
        
        # delete from oldest
        files_to_delete = len(image_files) - MAX_IMAGES
        for i in range(files_to_delete):
            os.remove(image_files[i])

#Flask----------------------------------------------------------------------------
@app.route("/")
def index():
    """main page with latesst image and weather data"""
    latest_image = get_latest_image()
    images = [latest_image] if latest_image else []
    total_images = len(get_image_list())
    
    return render_template('index.html', 
                         images=images,
                         latest_image=latest_image['filename'] if latest_image else None, # paste filenames into html 
                         latest_timestamp=latest_image['timestamp'] if latest_image else None,
                         latest_weather=latest_weather, # changin all the time latest record
                         total_images=total_images) 

@app.route("/gallery")
def gallery():
    """gallery with all images stored"""
    images = get_image_list()
    latest_image = images[0] if images else None
    
    return render_template('gallery.html', 
                         images=images,
                         latest_image=latest_image['filename'] if latest_image else None,
                         latest_timestamp=latest_image['timestamp'] if latest_image else None,
                         latest_weather=latest_weather,
                         total_images=len(images))

@app.route("/images/<filename>")
def serve_image(filename):
    """gives the image file from folder to browser - flask utility"""
    return send_from_directory(IMAGES_FOLDER, filename)

@app.route("/upload", methods=["POST"])
def upload():
    """Receive image from ESP32-CAM - the esp32 calls this route to upload image"""
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"{timestamp}.jpg"
    filepath = os.path.join(IMAGES_FOLDER, filename) # strip the data from request and save it to file
    
    with open(filepath, "wb") as f: 
        f.write(request.data) # stores image
    
    print(f"Image saved: {filename}")
    #checks for cleanup of old images
    if AUTOCLEANUP:
        cleanup_old_images()
    
    return "Image saved", 200 #200 means success to esp32cam

@app.route("/capture", methods=["POST"])
def capture():
    """sending capture request to esp32cam"""
    try:
        response = requests.get(f"http://{ESP32_IP}/capture", timeout=10) # post capture
        if response.status_code == 200:
            return {"success": True, "message": "successfully captured"}, 200
        else:
            return {"success": False, "message": "espcam error"}, 500 #esp32 unable to upload image
    except Exception as e:
        return {"success": False, "message": f"Error: {str(e)}"}, 500 #error wifi connection or other

@app.route("/weather", methods=["POST"])
def receive_weather():
    """recieve weather data called by esp32cam"""
    try:
        data = request.get_json()
        if not data:
            return {"error": "no data received"}, 400
        
        temperature = data.get('temperature') # get data from json
        humidity = data.get('humidity')
        pressure = data.get('pressure')
        
        #handle wrong format
        if temperature is None or humidity is None or pressure is None:
            return {"error": "missing data placeholders"}, 400 
        
        add_weather_record(temperature, humidity, pressure)
        
        return {"success": True, "message": "Data stored"}, 200
        
    except Exception as e:
        return {"error": str(e)}, 500 #some other error

@app.route("/api/weather")
def api_weather():
    return jsonify(latest_weather if latest_weather else {})

@app.route("/api/latest")
def api_latest(): #called by js for latest img info
    """latest image info"""
    latest = get_latest_image()
    return jsonify({
        "filename": latest['filename'] if latest else None,
        "timestamp": latest['timestamp'] if latest else None,
        "mtime": latest['mtime'] if latest else 0
    })

@app.route("/weather-history")
def weather_history():
    """render weather history page"""
    return render_template('weather_history.html')

@app.route("/weather-data")
def weather_data():
    # maybe in a future limit how many records return
    recent_data =  weather_data
    return jsonify(recent_data)

@app.route("/refresh-weather", methods=["POST"])
def refresh_weather():
    try:
        # Trigger ESP32 to take new weather measurement
        response = requests.get(f"http://{ESP32_IP}/weather", timeout=10)
        if response.status_code == 200:
            # Wait for ESP32 to POST new weather (not strictly needed, as ESP32 should send it automatically)
            # That was the problem since i didnot include the required headers in esp32 code so the browser ignored that  
            # next time include Access-Control-Allow-Origin on the esp32cam module so   
            load_weather_data()
            return jsonify({"success": True, "message": "Weather refreshed", "weather": latest_weather}), 200
        else:
            return jsonify({"success": False, "message": "ESP32 did not respond"}), 500
    except Exception as e:
        return jsonify({"success": False, "message": str(e)}), 500


if __name__ == '__main__':
    load_weather_data()
    app.run(host='0.0.0.0', port=5000)
