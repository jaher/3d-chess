/*
 * 3D Chess — Meta Ray-Ban Display Web App: engine Web Worker.
 *
 * Runs the alpha-beta search off the main thread so the 600x600 HUD keeps
 * rendering and the clock keeps ticking while the AI thinks. The page posts
 * { type:'search', state, opts }; the worker replies { type:'bestmove', move,
 * evalCp, depth }. The rules + engine are the SAME files the page loads, pulled
 * in here via importScripts so there is a single source of truth.
 */
'use strict';
importScripts('chess.js', 'engine.js');

self.onmessage = function (e) {
  const msg = e.data || {};
  if (msg.type === 'search') {
    const res = self.ChessEngine.searchBestMove(msg.state, msg.opts || {});
    self.postMessage({ type: 'bestmove', move: res.move, evalCp: res.evalCp, depth: res.depth, nodes: res.nodes });
  } else if (msg.type === 'eval') {
    // Static eval of a position (white-relative cp) for move-quality scoring.
    self.postMessage({ type: 'eval', evalCp: self.ChessEngine.evaluate(msg.state), tag: msg.tag });
  }
};
