#pragma once
#include "core/core.hpp"
#include <cstring>
namespace adapter {
using reaweb::Json;
struct Host : reaweb::Host {
  std::function<int(void*)> count_tracks;
  std::function<void*(void*, int)> get_track;
  std::function<std::string(void*)> track_name;
  std::function<double(void*, const std::string&)> get_track_value;
  std::function<bool(void*, const std::string&, double)> set_track_value;
  std::function<std::string()> version;
  std::function<std::string(void*)> project_name;
  std::function<double()> cursor_position, play_position, tempo;
  std::function<int()> play_state;
  std::function<void(double, bool, bool)> set_cursor;
  std::function<Json(void*, double)> time_to_beats;
  std::function<Json(void*)> count_markers;
  std::function<Json(void*, int)> enum_marker;
  std::function<int(void*)> track_color, track_fx_count;
  std::function<void(void*, int)> set_track_color;
  std::function<int(int, int, int)> color_to_native;
  std::function<Json(int)> color_from_native;
  std::function<Json(void*, int)> track_fx_name;
  std::function<int(void*, int)> track_fx_num_params;
  std::function<Json(void*, int, int)> track_fx_param;
};
inline Host* host;
inline void* proj(void* p) { return p ? p : host->current_project(); }
inline void* resolve(const char* name) {
#define FN(n, lambda) if (!std::strcmp(name, n)) return reinterpret_cast<void*>(+lambda)
  FN("CountTracks", [](void* p) { return host->count_tracks(proj(p)); });
  FN("CountSelectedTracks", [](void* p) { return host->count_selected_tracks(proj(p)); });
  FN("GetTrack", [](void* p, int i) { return host->get_track(proj(p), i); });
  FN("GetSelectedTrack", [](void* p, int i) { return host->get_selected_track(proj(p), i); });
  FN("ValidatePtr", [](void* t, const char* type) { return !std::strcmp(type,"ReaProject*") ? t == host->current_project() : host->valid_track(host->current_project(),t); });
  FN("ValidatePtr2", [](void* p, void* t, const char* type) { return !std::strcmp(type,"ReaProject*") ? t == host->current_project() : host->valid_track(proj(p),t); });
  FN("GetTrackGUID", [](void* t) -> void* { static reaweb::Guid g; g=host->track_guid(t); return g.data(); });
  FN("GetTrackName", [](void* t, char* b, int sz) { auto s=host->track_name(t); if (s.size()+1>static_cast<size_t>(sz)) return false; std::memcpy(b,s.c_str(),s.size()+1); return true; });
  FN("GetMediaTrackInfo_Value", [](void* t,const char* k) { return host->get_track_value(t,k); });
  FN("SetMediaTrackInfo_Value", [](void* t,const char* k,double v) { return host->set_track_value(t,k,v); });
  FN("GetAppVersion", []() { static std::string v; v=host->version(); return v.c_str(); });
  FN("GetProjectName", [](void* p,char* b,int sz) { auto s=host->project_name(proj(p)); std::memcpy(b,s.c_str(),std::min(s.size()+1,static_cast<size_t>(sz))); });
  FN("GetProjectStateChangeCount", [](void* p) { return host->change_count(proj(p)); });
  FN("GetCursorPosition", []() { return host->cursor_position(); });
  FN("GetPlayPosition", []() { return host->play_position(); });
  FN("GetPlayState", []() { return host->play_state(); });
  FN("Master_GetTempo", []() { return host->tempo(); });
  FN("SetEditCurPos", [](double t,bool m,bool s) { host->set_cursor(t,m,s); });
  FN("TimeMap2_timeToBeats", [](void* p,double t,int* m,int* c,double* f,int* d) { auto r=host->time_to_beats(proj(p),t); *m=r[1];*c=r[2];*f=r[3];*d=r[4];return r[0].get<double>(); });
  FN("CountProjectMarkers", [](void* p,int* m,int* r) { auto a=host->count_markers(proj(p));*m=a[1];*r=a[2];return a[0].get<int>(); });
  FN("EnumProjectMarkers3", [](void* p,int i,bool* r,double* pos,double* end,const char** n,int* idx,int* col) { auto a=host->enum_marker(proj(p),i);static std::string s;s=a[4];*r=a[1];*pos=a[2];*end=a[3];*n=s.c_str();*idx=a[5];*col=a[6];return a[0].get<int>(); });
  FN("GetTrackColor", [](void* t) { return host->track_color(t); });
  FN("SetTrackColor", [](void* t,int c) { host->set_track_color(t,c); });
  FN("ColorToNative", [](int r,int g,int b) { return host->color_to_native(r,g,b); });
  FN("ColorFromNative", [](int c,int* r,int* g,int* b) { auto a=host->color_from_native(c);*r=a[0];*g=a[1];*b=a[2]; });
  FN("TrackFX_GetCount", [](void* t) { return host->track_fx_count(t); });
  FN("TrackFX_GetNumParams", [](void* t,int fx) { return host->track_fx_num_params(t,fx); });
  FN("TrackFX_GetFXName", [](void* t,int fx,char* b,int sz) { auto a=host->track_fx_name(t,fx);auto s=a[1].get<std::string>();std::memcpy(b,s.c_str(),std::min(s.size()+1,static_cast<size_t>(sz)));return a[0].get<bool>(); });
  FN("TrackFX_GetParam", [](void* t,int fx,int p,double* lo,double* hi) { auto a=host->track_fx_param(t,fx,p);*lo=a[1];*hi=a[2];return a[0].get<double>(); });
#undef FN
  return nullptr;
}
inline void connect(Host& h) { host=&h; h.native_function=resolve; }
}
