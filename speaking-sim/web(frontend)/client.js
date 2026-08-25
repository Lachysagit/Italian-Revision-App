const transcript = document.getElementById("transcript");
const startButton = document.getElementById("start");
const doneButton = document.getElementById("done");
const endButton = document.getElementById("end");
//get references to HTML elements by their ID's

let socket = null;
let audioContext = null;
let mediaStream = null;
let micSource = null;
let processor = null;

let playbackSampleRate = null;
//rate the server says its PCM was synthesised at, read off the text message
//that arrives just before each binary audio frame

let captureState = "idle";

let headCut = 0;
//first sample of the pending block that belongs to the student
let tailCut = 0;
//one past the last sample of the pending block that belongs to the student

let blockStartTime = 0;


let pendingStop = false;


let pendingAudio = false;


let turnState = "idle";
//"idle" no session; "thinking" examiner is working and the mic is muted;
//"armed" student's turn, mic live and frames streaming

const CAPTURE_SAMPLE_RATE = 16000;
//whisper.cpp only accepts 16 kHz mono, so the mic is captured at that rate

const BLOCK_SIZE = 4096;
//the ScriptProcessor block length, used to clamp the cut points

function addLog(text) { //status or text from the server, for the operator only
    console.log(text);
}

const ROLE_LABELS = {
    examiner: "Examiner",
    student: "You",
};
//the speaker is a fixed key rather than free text so a caller cannot invent a
//role that lands unstyled, or spell one two ways and split it into two looks

function addTurn(role, text) {
    const label = ROLE_LABELS[role];
    if (!label) {
        addLog(`ignored turn from unknown role: ${role}`);
        return;
        //an unrecognised role means the caller is wrong, so refuse to paint
        //rather than show a turn whose speaker the student cannot identify
    }

    const card = document.createElement("div");
    card.className = `turn ${role}`;

    const heading = document.createElement("div");
    heading.className = "turn-role";
    heading.textContent = label;

    const body = document.createElement("div");
    body.className = "turn-text";
    body.textContent = text;

    card.appendChild(heading);
    card.appendChild(body);
    transcript.appendChild(card);
}

function setTurnState(state) {
    turnState = state;
    startButton.disabled = state !== "idle";
    doneButton.disabled = state !== "armed";
    endButton.disabled = state === "idle";
}

startButton.onclick = async () => {
    if (turnState !== "idle") {
        return;
    
    }
    setTurnState("thinking");

    try {
        audioContext = new AudioContext({ sampleRate: CAPTURE_SAMPLE_RATE });
    
        mediaStream = await navigator.mediaDevices.getUserMedia({ audio: true });
        //ask for audio only permission to get input
    } catch (error) {
        addLog(`microphone unavailable: ${error.message}`);
        teardown();
        return;
        //a denied permission previously left the socket open and a server-side
        //Session allocated for a client that could never speak
    }

    buildCaptureGraph();



    socket = new WebSocket(`ws://${location.host}/ws`);
    socket.binaryType = "arraybuffer";
    //tell the socket to send binary data as an ArrayBuffer (raw bytes)

    socket.onopen = () => {
        addLog("connected");
        socket.send(JSON.stringify({ type: "start", payload: "" }));
        //ask the examiner for the opening question. Without this nothing is
        //sent until the student ends a turn, so the exam begins in silence
    };
    socket.onclose = () => {
        addLog("disconnected");
        teardown();
        //a server-side drop must release the mic and the graph too, otherwise
        //the recording light stays on with nowhere to send the audio
    };
    socket.onerror = () => addLog("socket error");
    socket.onmessage = handleMessage;
};

function buildCaptureGraph() { //capture audio from users mic in browser
    micSource = audioContext.createMediaStreamSource(mediaStream);
    //create a source (mic) from mediaDevices
    processor = audioContext.createScriptProcessor(BLOCK_SIZE, 1, 1);
    //create a processing node of (buffer size, input channel, output channel)

    micSource.connect(processor);   //Connect mic audio to processor
    processor.connect(audioContext.destination);
  

    blockStartTime = audioContext.currentTime;
    //the block handed to the first callback starts filling as the graph is built

    processor.onaudioprocess = (event) => {
    //assign a function to on audio process event
        if (!audioContext) {
            return;
            //a callback queued before teardown ran
        }

        const input = event.inputBuffer.getChannelData(0);
        //getChannelData(0) accesses channel 0 samples as 32 bit floats
        let slice = null;

        if (captureState === "capturing") {
            slice = input;
            //a whole block, the common case
        } else if (captureState === "armed") {
            slice = input.subarray(headCut);
            //the first partial block: [0, headCut) is the examiner still coming
            //out of the speakers, everything after it is the student
            headCut = 0;
            captureState = "capturing";
        } else if (captureState === "stopping") {
            slice = input.subarray(headCut, tailCut);

            headCut = 0;
            captureState = "idle";
        }

        if (slice && slice.length) {
            sendPcm(slice);
        }

        if (pendingStop && captureState === "idle") {
            if (socket && socket.readyState === WebSocket.OPEN) {
                socket.send(JSON.stringify({ type: "stop", payload: "" }));
            }
            pendingStop = false;
            //sent only once the trimmed block above has gone out, so the whole
            //answer is on the server before handle_control reads it
        }

        blockStartTime = audioContext.currentTime;
        
    };
}

function sendPcm(floatSamples) { //forward one slice of mic audio to the server
    if (!socket || socket.readyState !== WebSocket.OPEN) {
        return;
    } //only send audio if the socket is ready

    const resampled = downsampleTo16k(floatSamples, audioContext.sampleRate);

    socket.send(floatToInt16(resampled).buffer);
    //rewrite samples as 16 bit Int and send the raw bytes
}

function cutPoint() { //how far into the block now filling we are, in samples
    const elapsed = audioContext.currentTime - blockStartTime;
    const sample = Math.round(elapsed * audioContext.sampleRate);
    //in context-rate samples, because that is what indexes inputBuffer. The
    //context is usually already at 16 kHz, but a browser may refuse the hint
    return Math.max(0, Math.min(BLOCK_SIZE, sample));
}

function armMic() { //hand the turn to the student
    if (turnState === "idle" || !audioContext) {
        return;
        //the session was ended while the examiner's audio was still playing, so
        //onended fired against a torn-down graph
    }
    if (captureState === "armed" || captureState === "capturing") {
        return;
        //already the student's turn; re-arming would drag headCut into the
        //middle of their words
    }

    headCut = cutPoint();
    captureState = "armed";
    setTurnState("armed");
    addLog("your turn - press Finished Response when you have finished");
}

function stopMic() { //take the turn back from the student
    if (captureState !== "armed" && captureState !== "capturing") {
        return;
    }
    if (captureState === "capturing") {
        headCut = 0;
        //the armed block has already flushed, so the whole head of this block
        //belongs to the student
    }

    tailCut = cutPoint();
    captureState = "stopping";
    pendingStop = true;
    //the stop message goes out from onaudioprocess once the trimmed block has
    //been sent, never before it
}

doneButton.onclick = () => { //the student has finished this answer
    if (turnState !== "armed") {
        return;
    }

    stopMic();
    setTurnState("thinking");
    addLog("thinking...");
};

endButton.onclick = () => {
    addLog("session ended");
    teardown();
};

function teardown() { //release everything this session allocated
    captureState = "idle";
    headCut = 0;
    tailCut = 0;
    pendingStop = false;
    pendingAudio = false;

    if (processor) {
        processor.onaudioprocess = null;
        processor.disconnect();
        processor = null;
    }
    if (micSource) {
        micSource.disconnect();
        micSource = null;
    }
    if (mediaStream) {
        const tracks = mediaStream.getTracks();
        //get all the tracks in the mic stream (usually one audio track)
        for (const track of tracks) {
            track.stop();
            //release the microphone and turn off the browser's recording light
        }
        mediaStream = null;
    }
    if (audioContext) {
        audioContext.close();
        audioContext = null;
        //close() frees the audio thread. Without it every abandoned session
        //left a context running for the lifetime of the page
    }
    if (socket) {
        const dying = socket;
        socket = null;
        dying.onclose = null;
        //teardown is already running, so do not let close() re-enter it
        if (dying.readyState === WebSocket.OPEN ||
            dying.readyState === WebSocket.CONNECTING) {
            dying.close();
            //closing makes Crow's .onclose erase the server-side Session
        }
    }

    playbackSampleRate = null;
    setTurnState("idle");
    //teardown is idempotent: every branch is null-guarded and nulls what it
    //released, so calling it from both endButton and socket.onclose is safe
}

function handleMessage(event) { //message from server
    if (typeof event.data === "string") {
        const message = JSON.parse(event.data);
        //parse JSON string into object
        addLog(`${message.type}: ${message.payload}`);
        //add to log element the message object
        if (message.sample_rate) {
            playbackSampleRate = message.sample_rate;
            //the server sends this with the examiner reply, immediately before
            //the binary frame it describes
        }

        if (message.type === "transcript") {
            addTurn("student", message.payload);
    
        }

        if (message.type === "examiner_text") {
            if (message.payload) {
                addTurn("examiner", message.payload);
            }
    

            if (message.sample_rate) {
                pendingAudio = true;
    
            } else {
                pendingAudio = false;
                armMic();
    
            }
        }

        if (message.type === "status" && message.payload === "busy") {
            armMic();
    
        }
    } else { //handle binary audio
        playAudio(event.data);
    }
}

function playAudio(arrayBuffer) { //handling audio from server
    const int16arrfromserver = new Int16Array(arrayBuffer);
    //create a new int16 array holding audio buffer sent by server
    if (int16arrfromserver.length === 0) {
        pendingAudio = false;
        armMic();
        return;
        //the TTS stub returns no samples, and createBuffer rejects a length of
        //0. There is nothing to wait for, so the turn passes to the student now
    }
    if (!audioContext) {
        return;
        //a frame that raced the teardown
    }

    captureState = "idle";
    setTurnState("thinking");
    //re-mute in case the mic was armed just before this frame landed: the
    //examiner is about to speak and must not be recorded as the answer

    const floatSamples = new Float32Array(int16arrfromserver.length);
    //create new array to hold 32 bit floats of audio (browser compatible)
    for (let i = 0; i < int16arrfromserver.length; i++) { //loop through
        floatSamples[i] = int16arrfromserver[i] / 32767;
        //convert each int 16 to float 32
    }

    const rate = playbackSampleRate || audioContext.sampleRate;
    //use the rate the server synthesised at. The AudioContext runs at the
    //capture rate, so falling back to it plays the reply at the wrong pitch
    const buffer = audioContext.createBuffer(1, floatSamples.length, rate);
    //empty mono buffer, length of the input buffer's samples. An AudioBuffer
    //may carry a different rate to its context and is resampled on playback
    buffer.getChannelData(0).set(floatSamples);
    //get reference to channel 0 in sample array, then .set copies all samples over

    const source = audioContext.createBufferSource();
    //create a buffer source node
    source.buffer = buffer; //give the source node the filled audio buffer
    source.connect(audioContext.destination); //connect to speakers

    source.onended = () => {
        armMic();
        //THE RE-ARM. The turn returns to the student only once the examiner has
        //stopped speaking, so the TTS output is never fed back into the mic
    };

    source.start(); //playback immediately
}

function downsampleTo16k(floatSamples, inputRate) {
    //helper func to bring mic samples down to the rate the STT model needs
    if (inputRate === CAPTURE_SAMPLE_RATE) {
        return floatSamples;
        //the AudioContext honoured the hint, so there is nothing to do
    }

    const ratio = inputRate / CAPTURE_SAMPLE_RATE;
    //how many input samples make up one output sample, e.g. 48000/16000 = 3
    const outputLength = Math.floor(floatSamples.length / ratio);
    const resampled = new Float32Array(outputLength);

    for (let i = 0; i < outputLength; i++) { //loop through output samples
        const position = i * ratio;
        //where this output sample sits in the input, usually between two samples
        const lower = Math.floor(position);
        const upper = Math.min(lower + 1, floatSamples.length - 1);
        const weight = position - lower;
        //how far between the two input samples the position falls, 0.0 to 1.0

        resampled[i] = floatSamples[lower] * (1 - weight) + floatSamples[upper] * weight;
        //linear interpolation between the two neighbours
    }
    return resampled;
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
