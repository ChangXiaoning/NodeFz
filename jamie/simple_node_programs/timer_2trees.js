/*
   This program launches a series of timers. 
   Its behavior should be deterministic.
   I'd like to see what kinds of CBs are invoked in a timer-only world.
*/

var timer_invocations = 0;
var n_timers = 0;
var max_timers = 40;

var timerFunc = function(){
  timer_invocations++;
  console.log("APP: Timer " + timer_invocations + " n_timers " + n_timers + " went off");
  for (var i = 0; i < 2; i++)
  {
    if (n_timers < max_timers)
    {
      n_timers++;
      setTimeout(timerFunc, 10);
    }
  }
};

n_timers++;
setTimeout(timerFunc, 50);
setTimeout(timerFunc, 50);

//Start reading from stdin so we don't exit.
process.stdin.resume();
