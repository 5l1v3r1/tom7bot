import flash.display.*;
class GameOver extends MovieClip {

  var endmusic : Sound;

  // Count of frames that have passed
  var frames : Number = 0;
  // If >= 0, then we're starting the game, but fading out
  var starting : Number = -1;

  // fastmode!
  var FADEFRAMES = 10;
  var ALPHAMULT = 10;

  var FADEOUTFRAMES = 20;
  var ALPHAOUTMULT = 5;

  public function onLoad() {
    Key.addListener(this);

    endmusic = new Sound(this);
    endmusic.attachSound('freedom.mp3');
    endmusic.setVolume(100);
    endmusic.start(0, 99999);
  }

  public function onEnterFrame() {
    // Fade in...
    frames++;
    if (frames < FADEFRAMES) {
      // XXX endmusic.setVolume(frames);
    }

    var alpha = 100;
    if (frames < FADEFRAMES) {
      alpha = frames * ALPHAMULT;
    }

    if (frames > FADEFRAMES) {
      if (starting > 0) {
        endmusic.setVolume(starting * ALPHAOUTMULT);
        starting--;
        alpha = starting * ALPHAOUTMULT;
        if (!starting) {
          reallyStart();
        }
      }
    }

    this._alpha = 100 - alpha;
  }

  // Called from TitleLaser when it's been on the
  // start button long enough.
  public function triggerStart() {
    // Only if we've been on the screen for 1 second.
    if (frames > 32) {
      starting = FADEOUTFRAMES;
    }
  }

  public function onKeyDown() {
    var k = Key.getCode();

    switch(k) {
    case 32: // space
    case 38: // up
      triggerStart();
      break;
    }
  }

  // XXX probably don't need to allow this...
  public function reallyStart() {
    Key.removeListener(this);
    trace('reallystartover!');
    // Stop music!

    this.endmusic.stop();

    this.swapDepths(0);
    this.removeMovieClip();

    // Whole game takes place on this blank frame
    // in the root timeline.
    _root.gotoAndStop('menu');
  }

};
