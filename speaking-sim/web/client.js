const log = document.getElementById("log");
const startButton = document.getElementById("start");
const stopButton = document.getElementById("stop");
//get references to HTML elements by their ID's

let socket = null;
let audioContext = null;
let mediaStream = null;

function addLog(text) { //update log element with status or text from server
    const line = document.createElement("div");
    line.textContent = text; //create a new div element with text
    log.appendChild(line); //append the text to the preexisting log element
}

startButton.onclick = async () => {
    socket = new WebSocket(`ws://${location.host}/ws`);
    //create websocket which is started by a click of the start button
    //browser sends HTTP request with headers requesting to upgrade to WebSocket
    //CROW receives bytes and matches path to its route to create connection

    socket.binaryType = "arraybuffer";
    //tell the socket to send binary data as a ArrayBuffer (raw bytes)

    socket.onopen = () => addLog("connected");
    //browser calls this when WebSocket handshake completes successfully
    socket.onclose = () => addLog("disconnected");
    //browser calls this when the connection ends
    socket.onmessage = handleMessage;
    //browser calls this every time the server sends data down the socket

    audioContext = new AudioContext();
    //create the Web Audio API's central object
    mediaStream = await navigator.mediaDevices.getUserMedia({ audio: true });
    //ask for audio only permission to get input

    startCapture();
};

function startCapture() { //capture audio from users mic in browser
    const source = audioContext.createMediaStreamSource(mediaStream);
    //create a source (mic) from mediaDevices
    const processor = audioContext.createScriptProcessor(4096, 1, 1);
    //create a processing node of (buffer size, input channel, output channel)

    source.connect(processor);   //Connect mic audio to processor
    processor.connect(audioContext.destination); //connect processor to output speakers
    


    processor.onaudioprocess = (event) => {
    //assign a function to on audio process event
        const floatSamples = event.inputBuffer.getChannelData(0);
        //get current audio buffers samples
        //getChannelData(0) accesses channel 0 samples as 32 bit floats
        const int16Samples = floatToInt16(floatSamples);
        //rewrite samples as 16 bit Int
        if (socket && socket.readyState === WebSocket.OPEN) {
            socket.send(int16Samples.buffer);
        } //only send audio if socket is ready
    };
}

function floatToInt16(floatSamples) { //helper func for browser audio samples to get sent to server
    const int16arrtoserver = new Int16Array(floatSamples.length);
    //create const array of 16 bit Ints at the length of audio samples
    for (let i = 0; i < floatSamples.length; i++)  //loop through audio samples
        {
        const cappedTop = Math.min(1, floatSamples[i]);
        //cap the upper bound: return whichever is smaller of 1 and i value

        const clamped = Math.max(-1, cappedTop);
        //floor the lower bound: return whichever is bigger -1 or cappedTop

        //clamped is now guaranteed to be within -1.0 to 1.0

        const scaled = clamped * 32767;
        //scale the [-1, 1] float onto the 16-bit integer range [-32767, 32767]

        int16arrtoserver[i] = scaled;
        //store the converted sample (Int16Array truncates any fraction to an integer)
        }
    return int16arrtoserver;
}

function handleMessage(event) { //message from server
    if (typeof event.data === "string") {
        const message = JSON.parse(event.data);
        //parse JSON string into object
        addLog(`${message.type}: ${message.payload}`);
        //add to log element the message object
    } else { //handle binary audio
        playAudio(event.data);
    }
}

function playAudio(arrayBuffer) { //handling audio from server
    const int16arrfromserver = new Int16Array(arrayBuffer);
    //create a new int16 array holding audio buffer sent by server
    const floatSamples = new Float32Array(int16.length);
    //create new array to hold 32 bit floats of audio (browser compatible)
    for (let i = 0; i < int16arrfromserver.length; i++) { //loop through
        floatSamples[i] = int16arrfromserver[i] / 32767; 
        //convert each int 16 to float 32 b
    }

    const buffer = audioContext.createBuffer(1, floatSamples.length, audioContext.sampleRate);
    //create empty audio buffer of 1 channel, length of input buffers samples and sample rate of browser
    buffer.getChannelData(0).set(floatSamples);
    //get reference to channel 0 in sample array, then .set copies all samples over

    const source = audioContext.createBufferSource();
    //create a buffer source node
    source.buffer = buffer; //give the source node the filled audio buffer
    source.connect(audioContext.destination); //connect to speakers
    source.start(); //playback immediately 
}

stopButton.onclick = () => { //stop button handler
    const message = JSON.stringify({ type: "stop", payload: "" });
    if (socket && socket.readyState === WebSocket.OPEN) {
        socket.send(message);
    }
    if (mediaStream) {
        const tracks = mediaStream.getTracks();
        //get all the tracks in the mic stream (usually one audio track) stored in an array

        for (const track of tracks) { //loop through tracks
            track.stop();
            //stop this track, releasing the microphone
        };
    }
};