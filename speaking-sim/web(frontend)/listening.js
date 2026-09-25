(function(){
  // The page is served at /listening, with no trailing slash, so a relative
  // "clips/..." would resolve against the site root. Prefixing here keeps each
  // manifest byte-identical to what build_manifest.py generates.
  function base(){ return '/listening/' + language + '/'; }

  var LANG_STORAGE = 'listeningLanguage';
  var LANGUAGES = {
    italian: {label: 'Italian', code: 'it'},
    german:  {label: 'German',  code: 'de'}
  };

  var audio   = document.getElementById('audio');
  var langSel = document.getElementById('language');
  var yearSel = document.getElementById('year');
  var out     = document.getElementById('out');
  var title   = document.getElementById('pageTitle');

  var language = localStorage.getItem(LANG_STORAGE);
  if (!LANGUAGES[language]) language = 'italian';

  var data = null;
  var current = null;   // row element currently loaded

  var ICON_PLAY  = '▶';
  var ICON_PAUSE = '‖';

  function fmt(s){
    if (s === null || s === undefined) return '';
    s = Math.round(s);
    return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
  }

  function load(row, restart){
    if (current && current !== row){
      current.classList.remove('playing');
      current.querySelector('.prog i').style.width = '0%';
      current.querySelector('.prog b').style.left = '0';
      current.querySelector('.meta').textContent = fmt(parseFloat(current.dataset.dur));
      current.querySelector('.play').textContent = ICON_PLAY;
    }
    // Assigning src does not reset readyState synchronously, so after a source change
    // the element can still report the outgoing clip's metadata. Track the swap here
    // rather than trusting readyState to tell us whether the new clip is ready.
    var swapped = false;
    if (current !== row || restart){
      audio.src = row.dataset.src;
      audio.currentTime = 0;
      swapped = true;
    }
    current = row;
    if (row.dataset.seek !== undefined){
      var want = parseFloat(row.dataset.seek);
      delete row.dataset.seek;
      var apply = function(){
        audio.removeEventListener('loadedmetadata', apply);
        if (audio.duration) audio.currentTime = want * audio.duration;
      };
      // Only seek immediately when the loaded clip is the one we are seeking in.
      if (!swapped && audio.readyState >= 1) apply();
      else audio.addEventListener('loadedmetadata', apply);
    }
    row.classList.add('playing');
    row.querySelector('.play').textContent = ICON_PAUSE;
    audio.play().catch(function(){
      row.querySelector('.play').textContent = ICON_PLAY;
    });
  }

  function toggle(row){
    if (current === row && !audio.paused){
      audio.pause();
    } else {
      load(row, false);
    }
  }

  function paint(row, frac, time, total){
    var pct = Math.max(0, Math.min(1, frac)) * 100;
    row.querySelector('.prog i').style.width = pct + '%';
    row.querySelector('.prog b').style.left = pct + '%';
    if (total) row.querySelector('.meta').textContent = fmt(time) + ' / ' + fmt(total);
  }

  function paintCurrent(){
    if (!current || !audio.duration) return;
    paint(current, audio.currentTime / audio.duration, audio.currentTime, audio.duration);
  }

  audio.addEventListener('timeupdate', paintCurrent);
  audio.addEventListener('seeked', paintCurrent);
  audio.addEventListener('loadedmetadata', paintCurrent);

  function rowDuration(row){
    if (current === row && audio.duration) return audio.duration;
    return parseFloat(row.dataset.dur) || 0;
  }

  // Seeking works whether or not the clip is the one already loaded: for an idle row
  // the target is remembered and applied once its audio reports a duration.
  function seekRow(row, frac){
    frac = Math.max(0, Math.min(1, frac));
    if (current === row && audio.duration){
      audio.currentTime = frac * audio.duration;
      paint(row, frac, audio.currentTime, audio.duration);
      return;
    }
    var dur = parseFloat(row.dataset.dur) || 0;
    paint(row, frac, frac * dur, dur || null);
    row.dataset.seek = frac;
    load(row, false);
  }

  // Returns the 0..1 position of a pointer event along the bar, or null when the event
  // carries no usable coordinates (e.g. a touchend, or a synthetic click at 0,0).
  function fracFromEvent(bar, e){
    var pt = (e.touches && e.touches[0]) || (e.changedTouches && e.changedTouches[0]) || e;
    if (!pt || typeof pt.clientX !== 'number') return null;
    var r = bar.getBoundingClientRect();
    if (!r.width) return null;
    return Math.max(0, Math.min(1, (pt.clientX - r.left) / r.width));
  }

  function attachScrub(bar, row){
    var dragging = false;
    var lastFrac = 0;   // last position the drag actually resolved to

    function move(e){
      if (!dragging) return;
      if (e.cancelable) e.preventDefault();
      var frac = fracFromEvent(bar, e);
      if (frac === null) return;
      lastFrac = frac;
      if (current === row && audio.duration){
        audio.currentTime = frac * audio.duration;
        paint(row, frac, audio.currentTime, audio.duration);
      } else {
        var dur = parseFloat(row.dataset.dur) || 0;
        paint(row, frac, frac * dur, dur || null);
      }
    }

    // A touchend carries no touch points, and a mouseup can arrive without usable
    // coordinates, which would read as x=0 and rewind the clip to the start. Commit
    // the last position the drag resolved to instead of re-reading the end event.
    function up(e){
      if (!dragging) return;
      dragging = false;
      bar.classList.remove('scrubbing');
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
      document.removeEventListener('touchmove', move);
      document.removeEventListener('touchend', up);
      var frac = fracFromEvent(bar, e);
      seekRow(row, frac === null ? lastFrac : frac);
    }

    function down(e){
      if (e.type === 'mousedown' && e.button !== 0) return;
      dragging = true;
      bar.classList.add('scrubbing');
      bar.focus();
      if (e.type === 'touchstart'){
        document.addEventListener('touchmove', move, {passive: false});
        document.addEventListener('touchend', up);
      } else {
        e.preventDefault();
        document.addEventListener('mousemove', move);
        document.addEventListener('mouseup', up);
      }
      move(e);
    }

    bar.addEventListener('mousedown', down);
    bar.addEventListener('touchstart', down, {passive: false});

    bar.addEventListener('keydown', function(e){
      if (e.key === 'ArrowRight')      nudge(row, e.shiftKey ? 1 : 5);
      else if (e.key === 'ArrowLeft')  nudge(row, e.shiftKey ? -1 : -5);
      else if (e.key === 'Home')       seekRow(row, 0);
      else if (e.key === 'End')        seekRow(row, 0.99);
      else return;
      e.preventDefault();
      e.stopPropagation();
    });
  }

  function nudge(row, secs){
    var dur = rowDuration(row);
    if (!dur) return;
    var at = (current === row && audio.duration) ? audio.currentTime : 0;
    seekRow(row, (at + secs) / dur);
  }

  audio.addEventListener('ended', function(){
    if (!current) return;
    current.querySelector('.play').textContent = ICON_PLAY;
    current.querySelector('.prog i').style.width = '100%';
  });

  audio.addEventListener('pause', function(){
    if (current) current.querySelector('.play').textContent = ICON_PLAY;
  });

  audio.addEventListener('play', function(){
    if (current) current.querySelector('.play').textContent = ICON_PAUSE;
  });

  // Transcript lines arrive as "Speaker: words", where the speaker may carry a
  // parenthesised gender for an inferred name, e.g. "Marco (male): Ciao".
  var TURN = /^([^:]{1,40}?)(?:\s*\(([a-z]+)\))?:\s*(.*)$/;

  function renderTranscript(host, text){
    host.textContent = '';
    if (!text){
      host.textContent = '(no transcript)';
      return;
    }
    text.split('\n').forEach(function(line){
      if (!line.trim()) return;
      var m = TURN.exec(line);
      var turn = document.createElement('div');
      turn.className = 'turn';
      if (m){
        var who = document.createElement('span');
        who.className = 'who';
        who.appendChild(document.createTextNode(m[1]));
        if (m[2]){
          if (m[2] === 'male' || m[2] === 'female'){
            who.classList.add(m[2]);
          }
          var g = document.createElement('span');
          g.className = 'g';
          g.textContent = m[2];
          who.appendChild(g);
        }
        var said = document.createElement('span');
        said.className = 'said';
        said.textContent = m[3];
        turn.appendChild(who);
        turn.appendChild(said);
      } else {
        var only = document.createElement('span');
        only.className = 'said';
        only.textContent = line;
        turn.appendChild(only);
      }
      host.appendChild(turn);
    });
  }

  // The exam paper's wording for a clip. Every part is optional: the familiarisation
  // text has none of it, and so does a language whose papers were never parsed, so an
  // item without these fields renders exactly as it did before questions existed.
  function renderQuestion(item){
    if (!item.stem && !item.options && !item.image_path) return null;

    var q = document.createElement('div');
    q.className = 'q';

    if (item.stem){
      var stem = document.createElement('p');
      stem.className = 'stem';
      stem.textContent = item.stem;
      if (item.marks){
        var marks = document.createElement('span');
        marks.className = 'marks';
        marks.textContent = item.marks + (item.marks === 1 ? ' mark' : ' marks');
        stem.appendChild(marks);
      }
      q.appendChild(stem);
    }

    if (item.options){
      var opts = document.createElement('ol');
      opts.className = 'opts';
      item.options.forEach(function(text){
        var li = document.createElement('li');
        li.textContent = text;
        opts.appendChild(li);
      });
      q.appendChild(opts);
    }

    // A picture or table question: the paper's own layout carries what the text cannot.
    if (item.image_path){
      var img = document.createElement('img');
      img.className = 'qimg';
      img.src = base() + item.image_path;
      img.alt = 'Question ' + (item.question_number || '') + ' as printed in the exam paper';
      img.loading = 'lazy';
      q.appendChild(img);
    }

    return q;
  }

  function makeRow(item, label){
    var row = document.createElement('div');
    row.className = 'row';
    row.dataset.src = base() + item.clip_path;

    var bar = document.createElement('div');
    bar.className = 'bar';

    var num = document.createElement('span');
    num.className = 'num';
    num.textContent = label;

    var play = document.createElement('button');
    play.className = 'play';
    play.textContent = ICON_PLAY;
    play.setAttribute('aria-label', 'Play ' + label);
    play.addEventListener('click', function(){ toggle(row); });

    var meta = document.createElement('span');
    meta.className = 'meta';
    meta.textContent = fmt(item.duration_seconds);
    row.dataset.dur = item.duration_seconds;

    var replay = document.createElement('button');
    replay.className = 'tbtn';
    replay.textContent = 'Replay';
    replay.addEventListener('click', function(){ load(row, true); });

    var tbtn = document.createElement('button');
    tbtn.className = 'tbtn';
    tbtn.textContent = 'Transcript';

    bar.appendChild(num);
    bar.appendChild(play);
    bar.appendChild(meta);
    bar.appendChild(replay);

    // The question sits open by default - it is what the student answers from, whereas
    // the transcript is the answer and stays hidden until asked for.
    var question = renderQuestion(item);
    if (question){
      var qbtn = document.createElement('button');
      qbtn.className = 'tbtn';
      qbtn.textContent = 'Hide question';
      qbtn.addEventListener('click', function(){
        question.classList.toggle('shut');
        qbtn.textContent = question.classList.contains('shut') ? 'Question' : 'Hide question';
      });
      bar.appendChild(qbtn);
    }

    bar.appendChild(tbtn);

    var prog = document.createElement('div');
    prog.className = 'prog';
    prog.appendChild(document.createElement('i'));
    prog.appendChild(document.createElement('b'));
    prog.tabIndex = 0;
    prog.setAttribute('role', 'slider');
    prog.setAttribute('aria-label', 'Seek ' + label);
    attachScrub(prog, row);

    var tx = document.createElement('div');
    tx.className = 'tx';
    renderTranscript(tx, item.transcript);

    tbtn.addEventListener('click', function(){
      tx.classList.toggle('open');
      tbtn.textContent = tx.classList.contains('open') ? 'Hide' : 'Transcript';
    });

    row.appendChild(bar);
    if (question) row.appendChild(question);
    row.appendChild(prog);
    row.appendChild(tx);
    return row;
  }

  function render(year){
    audio.pause();
    current = null;
    out.textContent = '';

    var y = data[year];
    if (!y){
      out.innerHTML = '<p class="msg">No data for this year.</p>';
      return;
    }

    if (y.familiarisation){
      var fs = document.createElement('section');
      fs.className = 'fam-section';
      var fh = document.createElement('h2');
      fh.className = 'sec';
      fh.textContent = 'Familiarisation — not part of the question set';
      fs.appendChild(fh);
      fs.appendChild(makeRow(y.familiarisation, 'Fam.'));
      out.appendChild(fs);
    }

    var qh = document.createElement('h2');
    qh.className = 'sec';
    qh.textContent = 'Questions (' + y.questions.length + ')';
    out.appendChild(qh);

    y.questions.forEach(function(q){
      out.appendChild(makeRow(q, String(q.question_number)));
    });
  }

  document.addEventListener('keydown', function(e){
    var t = e.target.tagName;
    if (t === 'INPUT' || t === 'SELECT' || t === 'TEXTAREA') return;
    if (e.code === 'Space'){
      e.preventDefault();
      if (current){
        toggle(current);
      } else {
        var first = document.querySelector('.row');
        if (first) toggle(first);
      }
    } else if (e.key === 'r' || e.key === 'R'){
      e.preventDefault();
      if (current) load(current, true);
    } else if (e.key === 'ArrowRight' || e.key === 'ArrowLeft'){
      if (!current) return;
      e.preventDefault();
      nudge(current, (e.key === 'ArrowRight' ? 1 : -1) * (e.shiftKey ? 1 : 5));
    }
  });

  yearSel.addEventListener('change', function(){ render(yearSel.value); });

  // Each language has its own manifest, so switching reloads rather than filters.
  function loadLanguage(){
    audio.pause();
    audio.removeAttribute('src');
    current = null;
    data = null;
    yearSel.textContent = '';
    title.textContent = LANGUAGES[language].label + ' Beginners — Listening Practice';
    out.innerHTML = '<p class="msg">Loading…</p>';

    var want = language;
    fetch(base() + 'manifest.json')
      .then(function(r){
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function(j){
        // A slow fetch can land after the picker moved on; that response is stale.
        if (want !== language) return;
        data = j;
        var years = Object.keys(j).sort().reverse();
        years.forEach(function(y){
          var o = document.createElement('option');
          o.value = y;
          o.textContent = y;
          yearSel.appendChild(o);
        });
        render(years[0]);
      })
      .catch(function(err){
        if (want !== language) return;
        out.innerHTML = '<p class="msg">Could not load the ' + LANGUAGES[want].label +
          ' clip index (' + err.message + ').' +
          '<br><br>The server reads it from <code>listening/' + want + '/manifest.json</code>, ' +
          'relative to its working directory, so it has to be started from the ' +
          '<code>speaking-sim</code> folder &mdash; that is what <code>run.ps1</code> does.</p>';
      });
  }

  initTranslate({language: LANGUAGES[language].code});
  //no log callback: the console log belongs to the exam page. Called before the
  //picker is wired, since it is what defines setTranslateLanguage

  langSel.value = language;
  langSel.addEventListener('change', function(){
    language = LANGUAGES[langSel.value] ? langSel.value : 'italian';
    localStorage.setItem(LANG_STORAGE, language);
    if (typeof savePreferredLanguage === 'function') savePreferredLanguage(language);
    //to the account as well as this browser, so the exam page and the next
    //device open on the same language
    setTranslateLanguage(LANGUAGES[language].code);
    loadLanguage();
  });

  // The account's language wins over whatever this browser last saved, the same
  // way it does on the exam page. Ignored when the account names a language this
  // page has no clips for, which leaves the picker where localStorage put it.
  window.applyListeningLanguage = function(id){
    if (!LANGUAGES[id] || id === language) return;
    language = id;
    localStorage.setItem(LANG_STORAGE, language);
    langSel.value = language;
    setTranslateLanguage(LANGUAGES[language].code);
    loadLanguage();
  };

  loadLanguage();
})();
