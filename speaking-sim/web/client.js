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
//the capture graph is built once per session and held here so that it can be
//disconnected on teardown. Previously the graph was local to startCapture()
//and unreachable afterwards, so it could never be shut down
let playbackSampleRate = null;
//rate the server says its PCM was synthesised at, read off the text message
//that arrives just before each binary audio frame

let captureState = "idle";
//"idle"      forwarding nothing
//"armed"     the turn began part-way through the block that is filling now, so
//            the samples before headCut are the tail of the examiner speaking
//            through the speakers and must be dropped
//"capturing" forwarding whole blocks
//"stopping"  the turn ended part-way through the block that is filling now, so
//            the samples from tailCut on were recorded after the button was
//            pressed and must be dropped
//
//the old boolean was read once per block, so a transition anywhere inside a
//block only took effect at the next block boundary: up to 256 ms of the
//student's first words were lost on arm, and up to 256 ms of examiner audio
//leaked in ahead of them

let headCut = 0;
//first sample of the pending block that belongs to the student
let tailCut = 0;
//one past the last sample of the pending block that belongs to the student

let blockStartTime = 0;
//audioContext.currentTime at the moment the block now filling began, which is
//when the previous onaudioprocess returned. The cut points are measured from
//here, so they come off the audio clock rather than a block boundary

let pendingStop = false;
//the stop message is held back until the trimmed final block has been sent, so
//the server cannot call take_audio() before the tail of the answer arrives

let pendingAudio = false;
//set when the examiner's text message carries a sample_rate, which is the
//server promising a binary frame. The mic is then armed by that frame's
//onended, so a slow link carrying real PCM can take as long as it likes

let turnState = "idle";
//"idle"     no session, nothing allocated
//"thinking" examiner is transcribing, replying or speaking; mic muted
//"armed"    student's turn; mic live and frames streaming

const CAPTURE_SAMPLE_RATE = 16000;
//whisper.cpp only accepts 16 kHz mono, so the mic is captured at that rate

const BLOCK_SIZE = 4096;
//the ScriptProcessor block length, used to clamp the cut points

function addLog(text) { //status or text from the server, for the operator only
    console.log(text);
    //this used to append to a #log div under the transcript, which put raw
    //protocol lines - "thinking...", "socket error", every examiner_text a
    //second time - on screen underneath the conversation cards. A student
    //reading the page saw each turn twice, once styled and once as a trace.
    //The diagnostics are still worth keeping, so they moved to the console
    //rather than being deleted: every existing addLog call site stays valid
    //and nothing in the turn logic had to change.
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
    //the student-facing conversation surface. Appending here is the assertion
    //"a turn happened", so only text the server actually committed to the
    //session may be passed in. Diagnostics go to addLog instead.
    //The two surfaces stay structurally separate rather than separated by
    //convention: this function is the only thing that appends to #transcript,
    //and addLog can no longer reach the DOM at all, so a diagnostic cannot
    //become a turn by way of some later refactor relaxing a predicate
}

function setTurnState(state) {
    turnState = state;
    startButton.disabled = state !== "idle";
    doneButton.disabled = state !== "armed";
    endButton.disabled = state === "idle";
    //the buttons are the guard as well as the display: Finished Response cannot
    //be pressed outside a student turn, and Start cannot be pressed while a
    //session is already up
}

startButton.onclick = async () => {
    if (turnState !== "idle") {
        return;
        //guards the double-Start case. Without this a second click overwrote
        //socket, audioContext and mediaStream while the first set was still
        //live: two sockets, two server sessions, two capture graphs, none of
        //the old ones reachable to be closed
    }
    setTurnState("thinking");
    //set immediately, not after the awaits, so a second click during the
    //permission prompt is rejected by the guard above

    try {
        audioContext = new AudioContext({ sampleRate: CAPTURE_SAMPLE_RATE });
        //ask for 16 kHz directly: the default follows the sound card, usually
        //44100 or 48000, and the server would then be handed audio at a rate
        //the STT model does not accept. Browsers that refuse the hint are
        //handled by downsampleTo16k below
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
    //built before the socket opens, so the graph is ready the moment the
    //examiner's opening question finishes playing

    socket = new WebSocket(`ws://${location.host}/ws`);
    //browser sends HTTP request with headers requesting to upgrade to WebSocket
    //CROW receives bytes and matches path to its route to create connection
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
    //the processor writes nothing to its output buffer, so this contributes
    //silence to the speakers. The connection is still required: Chrome stops
    //firing onaudioprocess on a ScriptProcessor whose output goes nowhere

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
            //the final partial block: keep [headCut, tailCut) and drop the rest.
            //headCut is normally 0, and is only non-zero when the student
            //finished inside the same block the turn was armed in
            headCut = 0;
            captureState = "idle";
        }
        //"idle" leaves slice null, so the examiner's thinking and speaking time
        //no longer arrives at the server as silent frames filling the 40 s cap
        //in session.cpp

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
        //the next block begins filling now, and the next transition measures
        //its cut point against this
    };
}

function sendPcm(floatSamples) { //forward one slice of mic audio to the server
    if (!socket || socket.readyState !== WebSocket.OPEN) {
        return;
    } //only send audio if the socket is ready

    const resampled = downsampleTo16k(floatSamples, audioContext.sampleRate);
    //a browser is free to ignore the 16 kHz hint given to the AudioContext,
    //so the actual rate is checked here and the samples brought down if needed
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
    //this both trims the block in flight at the exact sample the button was
    //pressed and defers the stop message until that block has been sent, so
    //the last word of the answer is neither clipped nor left stranded until
    //after handle_control has already called take_audio()
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
        //clearing the handler as well as disconnecting: a queued callback can
        //still run after disconnect(), and the gate above is what stops it
        //doing anything
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
            //the student's own answer as STT heard it. The server does not
            //send this yet - MessageType::Transcript is declared in the
            //protocol but never constructed - so this branch is dormant.
            //Until it is emitted, #transcript shows the examiner side only
            //and is NOT yet a faithful mirror of the session history
        }

        if (message.type === "examiner_text") {
            if (message.payload) {
                addTurn("examiner", message.payload);
            }
            //guarded on non-empty because a failed turn still sends an
            //examiner_text, with an empty payload, purely to re-arm the mic.
            //The server committed nothing to the session on that path, so
            //painting anything here would show a turn the history does not
            //contain. Empty payload paints nothing, which is the honest
            //representation of "that turn did not happen".
            //A synthesize() failure DOES carry text - the question was
            //committed - so it paints here and the student reads what they
            //would otherwise have heard

            if (message.sample_rate) {
                pendingAudio = true;
                //the server only sets sample_rate when a binary frame follows,
                //so stay muted: that frame arms the mic when it finishes
                //playing, however long it takes to arrive
            } else {
                pendingAudio = false;
                armMic();
                //no rate means no audio is coming, so there is nothing to wait
                //for and the turn passes to the student now. This replaces a
                //400 ms timer that only guessed at the same thing
            }
        }

        if (message.type === "status" && message.payload === "busy") {
            armMic();
            //the server refused this turn because a job was already in flight,
            //so no reply is coming. Without this the client would sit in
            //"thinking" forever with the Finished Response button disabled
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
    //re-mute in case the mic was armed just before this frame landed. The
    //examiner is about to speak through the speakers and must not be recorded
    //as the student's answer

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
    //create empty audio buffer of 1 channel, length of input buffers samples
    //an AudioBuffer may carry a different rate to its context; the browser
    //resamples it on playback
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
