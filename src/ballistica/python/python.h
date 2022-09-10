// Released under the MIT License. See LICENSE for details.

#ifndef BALLISTICA_PYTHON_PYTHON_H_
#define BALLISTICA_PYTHON_PYTHON_H_

#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ballistica/ballistica.h"
#include "ballistica/core/context.h"
#include "ballistica/core/object.h"
#include "ballistica/generic/buffer.h"
#include "ballistica/generic/runnable.h"
#include "ballistica/math/point2d.h"
#include "ballistica/platform/min_sdl.h"
#include "ballistica/python/python_ref.h"

namespace ballistica {

/// General python support/infrastructure class.
class Python {
 public:
  /// When calling a python callable directly, you can use the following
  /// to push and pop a text label which will be printed as 'call' in errors.
  class ScopedCallLabel {
   public:
    explicit ScopedCallLabel(const char* label) {
      prev_label_ = current_label_;
    }
    ~ScopedCallLabel() { current_label_ = prev_label_; }
    static auto current_label() -> const char* { return current_label_; }

   private:
    const char* prev_label_{};
    static const char* current_label_;
    BA_DISALLOW_CLASS_COPIES(ScopedCallLabel);
  };

  /// Use this to protect Python code that may be run in cases where we don't
  /// hold the Global Interpreter Lock (GIL) (basically anything outside of the
  /// game thread).
  class ScopedInterpreterLock {
   public:
    ScopedInterpreterLock();
    ~ScopedInterpreterLock();

   private:
    class Impl;
    // Note: should use unique_ptr for this, but build fails on raspberry pi
    // (gcc 8.3.0). Works on Ubuntu 9.3 so should try again later.
    // std::unique_ptr<Impl> impl_{};
    Impl* impl_{};
  };

  /// Return whether the current thread holds the global-interpreter-lock.
  /// We must always hold the GIL while running python code.
  /// This *should* generally be the case by default, but this can be handy for
  /// sanity checking that.
  static auto HaveGIL() -> bool;

  /// Attempt to print the python stack trace.
  static auto PrintStackTrace() -> void;

  /// Pass any PyObject* (including nullptr) to get a readable string
  /// (basically equivalent of str(foo)).
  static auto ObjToString(PyObject* obj) -> std::string;

  /// Given an asset-package python object and a media name, verify
  /// that the asset-package is valid in the current context and return
  /// its fully qualified name if so.  Throw an Exception if not.
  auto ValidatedPackageAssetName(PyObject* package, const char* name)
      -> std::string;

  static auto LogContextForCallableLabel(const char* label) -> void;
  static auto LogContextEmpty() -> void;
  static auto LogContextAuto() -> void;
  static auto LogContextNonLogicThread() -> void;
  Python();
  ~Python();

  auto Reset(bool init = true) -> void;

  /// Add classes to the newly created ba module.
  static auto InitModuleClasses(PyObject* module) -> void;
  static auto GetModuleMethods() -> std::vector<PyMethodDef>;

  auto GetContextBaseString() -> std::string;
  auto GetControllerValue(InputDevice* input_device,
                          const std::string& value_name) -> int;
  auto GetControllerFloatValue(InputDevice* input_device,
                               const std::string& value_name) -> float;
  auto HandleDeviceMenuPress(InputDevice* input_device) -> void;
  auto GetLastPlayerNameFromInputDevice(InputDevice* input_device)
      -> std::string;
  auto AcquireGIL() -> void;
  auto ReleaseGIL() -> void;

  auto LaunchStringEdit(TextWidget* w) -> void;
  auto CaptureGamePadInput(PyObject* obj) -> void;
  auto ReleaseGamePadInput() -> void;
  auto CaptureKeyboardInput(PyObject* obj) -> void;
  auto ReleaseKeyboardInput() -> void;
  auto HandleFriendScoresCB(const FriendScoreSet& ss) -> void;
  auto IssueCallInLogicThreadWarning(PyObject* call) -> void;

  /// Borrowed from python's source code: used in overriding of objects' dir()
  /// results.
  static auto generic_dir(PyObject* self) -> PyObject*;

  /// For use by g_game in passing events along to the python layer (for
  /// captured input, etc).
  auto HandleJoystickEvent(const SDL_Event& event,
                           InputDevice* input_device = nullptr) -> bool;
  auto HandleKeyPressEvent(const SDL_Keysym& keysym) -> bool;
  auto HandleKeyReleaseEvent(const SDL_Keysym& keysym) -> bool;

  auto inited() const -> bool { return inited_; }

  /// Filter incoming chat message from client.
  /// If returns false, message should be ignored.
  auto FilterChatMessage(std::string* message, int client_id) -> bool;

  /// Pass a chat message along to the python UI layer for handling..
  auto HandleLocalChatMessage(const std::string& message) -> void;

  /// Pop up an in-game window to show a url (NOT in a browser).
  auto ShowURL(const std::string& url) -> void;

  auto AddCleanFrameCommand(const Object::Ref<PythonContextCall>& c) -> void;
  auto RunCleanFrameCommands() -> void;

  /// Return a minimal filename/position string such as 'foo.py:201' based
  /// on the Python stack state. This shouldn't be too expensive to fetch and
  /// is useful as an object identifier/etc.
  static auto GetPythonFileLocation(bool pretty = true) -> std::string;

  auto set_env_obj(PyObject* obj) -> void { env_ = obj; }
  auto env_obj() const -> PyObject* {
    assert(env_);
    return env_;
  }
  auto main_dict() const -> PyObject* {
    assert(main_dict_);
    return main_dict_;
  }
  auto PlayMusic(const std::string& music_type, bool continuous) -> void;

  // Fetch raw values from the config dict. The default value is returned if
  // the requested value is not present or not of a compatible type.
  // Note: to get app config values you should generally use the bs::AppConfig
  // functions (which themselves call these functions)
  auto GetRawConfigValue(const char* name)
      -> PyObject*;  // (returns a borrowed ref)
  auto GetRawConfigValue(const char* name, const char* default_value)
      -> std::string;
  auto GetRawConfigValue(const char* name, float default_value) -> float;
  auto GetRawConfigValue(const char* name, std::optional<float> default_value)
      -> std::optional<float>;
  auto GetRawConfigValue(const char* name, int default_value) -> int;
  auto GetRawConfigValue(const char* name, bool default_value) -> bool;
  auto SetRawConfigValue(const char* name, float value) -> void;

  auto RunDeepLink(const std::string& url) -> void;
  auto GetResource(const char* key, const char* fallback_resource = nullptr,
                   const char* fallback_value = nullptr) -> std::string;
  auto GetTranslation(const char* category, const char* s) -> std::string;

  // For checking and pulling values out of Python objects.
  // These will all throw Exceptions on errors.
  static auto GetPyString(PyObject* o) -> std::string;
  static auto GetPyInt64(PyObject* o) -> int64_t;
  static auto GetPyInt(PyObject* o) -> int;
  static auto GetPyNode(PyObject* o, bool allow_empty_ref = false,
                        bool allow_none = false) -> Node*;
  static auto GetPyNodes(PyObject* o) -> std::vector<Node*>;
  static auto GetPyMaterials(PyObject* o) -> std::vector<Material*>;
  static auto GetPyTextures(PyObject* o) -> std::vector<Texture*>;
  static auto GetPyModels(PyObject* o) -> std::vector<Model*>;
  static auto GetPySounds(PyObject* o) -> std::vector<Sound*>;
  static auto GetPyCollideModels(PyObject* o) -> std::vector<CollideModel*>;
  static auto GetPyCollideModel(PyObject* o, bool allow_empty_ref = false,
                                bool allow_none = false) -> CollideModel*;
  static auto IsPySession(PyObject* o) -> bool;
  static auto GetPySession(PyObject* o) -> Session*;
  static auto IsPyString(PyObject* o) -> bool;
  static auto GetPyBool(PyObject* o) -> bool;
  static auto GetPyHostActivity(PyObject* o) -> HostActivity*;
  static auto IsPyHostActivity(PyObject* o) -> bool;
  static auto GetPyInputDevice(PyObject* o) -> InputDevice*;
  static auto IsPyPlayer(PyObject* o) -> bool;
  static auto GetPyPlayer(PyObject* o, bool allow_empty_ref = false,
                          bool allow_none = false) -> Player*;
  static auto GetPySessionPlayer(PyObject* o, bool allow_empty_ref = false,
                                 bool allow_none = false) -> Player*;
  static auto GetPyMaterial(PyObject* o, bool allow_empty_ref = false,
                            bool allow_none = false) -> Material*;
  static auto GetPyTexture(PyObject* o, bool allow_empty_ref = false,
                           bool allow_none = false) -> Texture*;
  static auto GetPyModel(PyObject* o, bool allow_empty_ref = false,
                         bool allow_none = false) -> Model*;
  static auto GetPySound(PyObject* o, bool allow_empty_ref = false,
                         bool allow_none = false) -> Sound*;
  static auto GetPyData(PyObject* o, bool allow_empty_ref = false,
                        bool allow_none = false) -> Data*;
  static auto GetPyWidget(PyObject* o) -> Widget*;
  static auto CanGetPyDouble(PyObject* o) -> bool;
  static auto GetPyFloat(PyObject* o) -> float {
    return static_cast<float>(GetPyDouble(o));
  }
  static auto GetPyDouble(PyObject* o) -> double;
  static auto GetPyFloats(PyObject* o) -> std::vector<float>;
  static auto GetPyInts64(PyObject* o) -> std::vector<int64_t>;
  static auto GetPyInts(PyObject* o) -> std::vector<int>;
  static auto GetPyStrings(PyObject* o) -> std::vector<std::string>;
  static auto GetPyUInts64(PyObject* o) -> std::vector<uint64_t>;
  static auto GetPyPoint2D(PyObject* o) -> Point2D;
  static auto CanGetPyVector3f(PyObject* o) -> bool;
  static auto GetPyVector3f(PyObject* o) -> Vector3f;

  static auto GetPyEnum_Permission(PyObject* obj) -> Permission;
  static auto GetPyEnum_SpecialChar(PyObject* obj) -> SpecialChar;
  static auto GetPyEnum_TimeType(PyObject* obj) -> TimeType;
  static auto GetPyEnum_TimeFormat(PyObject* obj) -> TimeFormat;
  static auto IsPyEnum_InputType(PyObject* obj) -> bool;
  static auto GetPyEnum_InputType(PyObject* obj) -> InputType;

  static auto GetNodeAttr(Node* node, const char* attribute_name) -> PyObject*;
  static auto SetNodeAttr(Node* node, const char* attr_name,
                          PyObject* value_obj) -> void;

  /// Set Python exception from C++ Exception.
  static auto SetPythonException(const Exception& exc) -> void;

  static auto DoBuildNodeMessage(PyObject* args, int arg_offset,
                                 Buffer<char>* b, PyObject** user_message_obj)
      -> void;
  auto DoNewNode(PyObject* args, PyObject* keywds) -> Node*;

  /// Identifiers for specific Python objects we grab references to for easy
  /// access.
  enum class ObjID {
    kEmptyTuple,
    kApp,
    kEnv,
    kDeepCopyCall,
    kShallowCopyCall,
    kShouldShatterMessageClass,
    kImpactDamageMessageClass,
    kPickedUpMessageClass,
    kDroppedMessageClass,
    kOutOfBoundsMessageClass,
    kPickUpMessageClass,
    kDropMessageClass,
    kShowURLWindowCall,
    kActivityClass,
    kSessionClass,
    kJsonDumpsCall,
    kJsonLoadsCall,
    kGetDeviceValueCall,
    kDeviceMenuPressCall,
    kGetLastPlayerNameFromInputDeviceCall,
    kOnScreenKeyboardClass,
    kFilterChatMessageCall,
    kHandleLocalChatMessageCall,
    kHandlePartyInviteRevokeCall,
    kDoPlayMusicCall,
    kDeepLinkCall,
    kGetResourceCall,
    kTranslateCall,
    kLStrClass,
    kCallClass,
    kGarbageCollectSessionEndCall,
    kConfig,
    kFinishBootstrappingCall,
    kClientInfoQueryResponseCall,
    kResetToMainMenuCall,
    kSetConfigFullscreenOnCall,
    kSetConfigFullscreenOffCall,
    kNotSignedInScreenMessageCall,
    kConnectingToPartyMessageCall,
    kRejectingInviteAlreadyInPartyMessageCall,
    kConnectionFailedMessageCall,
    kTemporarilyUnavailableMessageCall,
    kInProgressMessageCall,
    kErrorMessageCall,
    kPurchaseNotValidErrorCall,
    kPurchaseAlreadyInProgressErrorCall,
    kGearVRControllerWarningCall,
    kVROrientationResetCBMessageCall,
    kVROrientationResetMessageCall,
    kHandleAppResumeCall,
    kHandleLogCall,
    kLaunchMainMenuSessionCall,
    kLanguageTestToggleCall,
    kAwardInControlAchievementCall,
    kAwardDualWieldingAchievementCall,
    kPrintCorruptFileErrorCall,
    kPlayGongSoundCall,
    kLaunchCoopGameCall,
    kPurchasesRestoredMessageCall,
    kDismissWiiRemotesWindowCall,
    kUnavailableMessageCall,
    kSubmitAnalyticsCountsCall,
    kSetLastAdNetworkCall,
    kNoGameCircleMessageCall,
    kGooglePlayPurchasesNotAvailableMessageCall,
    kEmptyCall,
    kLevelIconPressCall,
    kTrophyIconPressCall,
    kCoinIconPressCall,
    kTicketIconPressCall,
    kBackButtonPressCall,
    kFriendsButtonPressCall,
    kPrintTraceCall,
    kToggleFullscreenCall,
    kPartyIconActivateCall,
    kReadConfigCall,
    kUIRemotePressCall,
    kQuitWindowCall,
    kRemoveInGameAdsMessageCall,
    kTelnetAccessRequestCall,
    kOnAppPauseCall,
    kQuitCall,
    kShutdownCall,
    kGCDisableCall,
    kShowPostPurchaseMessageCall,
    kContextError,
    kNotFoundError,
    kNodeNotFoundError,
    kSessionTeamNotFoundError,
    kInputDeviceNotFoundError,
    kDelegateNotFoundError,
    kSessionPlayerNotFoundError,
    kWidgetNotFoundError,
    kActivityNotFoundError,
    kSessionNotFoundError,
    kAssetPackageClass,
    kTimeFormatClass,
    kTimeTypeClass,
    kInputTypeClass,
    kPermissionClass,
    kSpecialCharClass,
    kPlayerClass,
    kGetPlayerIconCall,
    kLstrFromJsonCall,
    kUUIDStrCall,
    kHashStringsCall,
    kHaveAccountV2CredentialsCall,
    kLast  // Sentinel; must be at end.
  };

  /// Access a particular Python object we've grabbed/stored.
  auto obj(ObjID id) const -> const PythonRef& {
    assert(id < ObjID::kLast);
    if (g_buildconfig.debug_build()) {
      if (!objs_[static_cast<int>(id)].exists()) {
        throw Exception("Python::obj() called on nonexistent val "
                        + std::to_string(static_cast<int>(id)));
      }
    }
    return objs_[static_cast<int>(id)];
  }

  /// Return whether we have a particular Python object.
  auto objexists(ObjID id) const -> bool {
    assert(id < ObjID::kLast);
    return objs_[static_cast<int>(id)].exists();
  }

  /// Create a Python list of strings.
  auto StringList(const std::list<std::string>& values) -> PythonRef;

  /// Create a Python single-member tuple.
  auto SingleMemberTuple(const PythonRef& member) -> PythonRef;

  /// Push a call to a preset obj to the game thread
  /// (will be run in the UI context).
  auto PushObjCall(ObjID obj) -> void;

  /// Push a call with a single string arg.
  auto PushObjCall(ObjID obj, const std::string& arg) -> void;

  /// Register Python location and returns true if it has not
  /// yet been registered. (for print-once type stuff).
  auto DoOnce() -> bool;

  /// Check values passed to timer functions; triggers warnings
  /// for cases that look like they're passing milliseconds as seconds
  /// or vice versa... (can remove this once things are settled in).
  auto TimeFormatCheck(TimeFormat time_format, PyObject* length_obj) -> void;

 private:
  /// Check/set debug related initialization.
  auto SetupInterpreterDebugState() -> void;

  /// Set up system paths if needed (for embedded builds).
  auto SetupPythonHome() -> void;

  /// Set the value for a named object.
  auto StoreObj(ObjID id, PyObject* pyobj, bool incref = false) -> void;

  /// Set the value for a named object and verify that it is a callable.
  auto StoreObjCallable(ObjID id, PyObject* pyobj, bool incref = false) -> void;

  /// Set the value for a named object to the result of a Python expression.
  auto StoreObj(ObjID id, const char* expression, PyObject* context = nullptr)
      -> void;

  /// Set the value for a named object to the result of a Python expression
  /// and verify that it is callable.
  auto StoreObjCallable(ObjID id, const char* expression,
                        PyObject* context = nullptr) -> void;

  std::set<std::string> do_once_locations_;
  PythonRef objs_[static_cast<int>(ObjID::kLast)];
  bool inited_{};
  std::list<Object::Ref<PythonContextCall> > clean_frame_commands_;
  PythonRef game_pad_call_;
  PythonRef keyboard_call_;
  PyObject* empty_dict_object_{};
  PyObject* main_dict_{};
  PyObject* env_{};
  PyThreadState* thread_state_{};
};

}  // namespace ballistica

#endif  // BALLISTICA_PYTHON_PYTHON_H_
