const log = document.getElementById("log");
const startButton = document.getElementById("start");
const stopButton = document.getElementById("stop");
//get references to HTML elements by their ID's

let socket = null;
let audioContext = null;
let mediaStream = null;

function addLog(text) {
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

function startCapture() {
    const source = audioContext.createMediaStreamSource(mediaStream);
    const processor = audioContext.createScriptProcessor(4096, 1, 1);

    source.connect(processor);
    processor.connect(audioContext.destination);

    processor.onaudioprocess = (event) => {
        const floatSamples = event.inputBuffer.getChannelData(0);
        const int16Samples = floatToInt16(floatSamples);
        if (socket && socket.readyState === WebSocket.OPEN) {
            socket.send(int16Samples.buffer);
        }
    };
}