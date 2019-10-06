"""Some handy base class and special purpose Activity types."""
from __future__ import annotations

import time
from typing import TYPE_CHECKING

import _ba
from ba import _activity

if TYPE_CHECKING:
    from typing import Any, Dict, Optional
    import ba
    from ba._lobby import JoinInfo


class EndSessionActivity(_activity.Activity):
    """Special ba.Activity to fade out and end the current ba.Session."""

    def __init__(self, settings: Dict[str, Any]):
        super().__init__(settings)

        # Keeps prev activity alive while we fadeout.
        self.transition_time = 0.25
        self.inherits_tint = True
        self.inherits_slow_motion = True
        self.inherits_camera_vr_offset = True
        self.inherits_vr_overlay_center = True

    def on_transition_in(self) -> None:
        super().on_transition_in()
        _ba.fade_screen(False)
        _ba.lock_all_input()

    def on_begin(self) -> None:
        # pylint: disable=cyclic-import
        from bastd.mainmenu import MainMenuSession
        from ba._apputils import call_after_ad
        from ba._general import Call
        super().on_begin()
        _ba.unlock_all_input()
        call_after_ad(Call(_ba.new_host_session, MainMenuSession))


class JoiningActivity(_activity.Activity):
    """Standard activity for waiting for players to join.

    It shows tips and other info and waits for all players to check ready.
    """

    def __init__(self, settings: Dict[str, Any]):
        super().__init__(settings)

        # This activity is a special 'joiner' activity.
        # It will get shut down as soon as all players have checked ready.
        self.is_joining_activity = True

        # Players may be idle waiting for joiners; lets not kick them for it.
        self.allow_kick_idle_players = False

        # In vr mode we don't want stuff moving around.
        self.use_fixed_vr_overlay = True

        self._background: Optional[ba.Actor] = None
        self._tips_text: Optional[ba.Actor] = None
        self._join_info: Optional[JoinInfo] = None

    def on_transition_in(self) -> None:
        # pylint: disable=cyclic-import
        from bastd.actor.tipstext import TipsText
        from bastd.actor.background import Background
        from ba import _music
        super().on_transition_in()
        self._background = Background(fade_time=0.5,
                                      start_faded=True,
                                      show_logo=True)
        self._tips_text = TipsText()
        _music.setmusic('CharSelect')
        self._join_info = self.session.lobby.create_join_info()
        _ba.set_analytics_screen('Joining Screen')


class TransitionActivity(_activity.Activity):
    """A simple overlay fade out/in.

    Useful as a bare minimum transition between two level based activities.
    """

    def __init__(self, settings: Dict[str, Any]):
        super().__init__(settings)

        # Keep prev activity alive while we fade in.
        self.transition_time = 0.5
        self.inherits_slow_motion = True  # Don't change.
        self.inherits_tint = True  # Don't change.
        self.inherits_camera_vr_offset = True  # Don't change.
        self.inherits_vr_overlay_center = True
        self.use_fixed_vr_overlay = True
        self._background: Optional[ba.Actor] = None

    def on_transition_in(self) -> None:
        # pylint: disable=cyclic-import
        from bastd.actor import background  # FIXME: Don't use bastd from ba.
        super().on_transition_in()
        self._background = background.Background(fade_time=0.5,
                                                 start_faded=False,
                                                 show_logo=False)

    def on_begin(self) -> None:
        super().on_begin()

        # Die almost immediately.
        _ba.timer(0.1, self.end)


class ScoreScreenActivity(_activity.Activity):
    """A standard score screen that fades in and shows stuff for a while.

    After a specified delay, player input is assigned to end the activity.
    """

    def __init__(self, settings: Dict[str, Any]):
        super().__init__(settings)
        self.transition_time = 0.5
        self._birth_time = _ba.time()
        self._min_view_time = 5.0
        self.inherits_tint = True
        self.inherits_camera_vr_offset = True
        self.use_fixed_vr_overlay = True
        self._allow_server_restart = False
        self._background: Optional[ba.Actor] = None
        self._tips_text: Optional[ba.Actor] = None
        self._kicked_off_server_shutdown = False
        self._kicked_off_server_restart = False

    def on_player_join(self, player: ba.Player) -> None:
        from ba import _general
        super().on_player_join(player)
        time_till_assign = max(
            0, self._birth_time + self._min_view_time - _ba.time())

        # If we're still kicking at the end of our assign-delay, assign this
        # guy's input to trigger us.
        _ba.timer(time_till_assign, _general.WeakCall(self._safe_assign,
                                                      player))

    def on_transition_in(self,
                         music: Optional[str] = 'Scores',
                         show_tips: bool = True) -> None:
        # FIXME: Unify args.
        # pylint: disable=arguments-differ
        # pylint: disable=cyclic-import
        from bastd.actor import tipstext
        from bastd.actor import background
        from ba import _music as bs_music
        super().on_transition_in()
        self._background = background.Background(fade_time=0.5,
                                                 start_faded=False,
                                                 show_logo=True)
        if show_tips:
            self._tips_text = tipstext.TipsText()
        bs_music.setmusic(music)

    def on_begin(self, custom_continue_message: ba.Lstr = None) -> None:
        # FIXME: Unify args.
        # pylint: disable=arguments-differ
        # pylint: disable=cyclic-import
        from bastd.actor import text
        from ba import _lang
        super().on_begin()

        # Pop up a 'press any button to continue' statement after our
        # min-view-time show a 'press any button to continue..'
        # thing after a bit.
        if _ba.app.interface_type == 'large':
            # FIXME: Need a better way to determine whether we've probably
            #  got a keyboard.
            sval = _lang.Lstr(resource='pressAnyKeyButtonText')
        else:
            sval = _lang.Lstr(resource='pressAnyButtonText')

        text.Text(custom_continue_message if custom_continue_message else sval,
                  v_attach='bottom',
                  h_align='center',
                  flash=True,
                  vr_depth=50,
                  position=(0, 10),
                  scale=0.8,
                  color=(0.5, 0.7, 0.5, 0.5),
                  transition='in_bottom_slow',
                  transition_delay=self._min_view_time).autoretain()

    def _player_press(self) -> None:

        # If we're running in server-mode and it wants to shut down
        # or restart, this is a good place to do it
        if self._handle_server_restarts():
            return
        self.end()

    def _safe_assign(self, player: ba.Player) -> None:

        # Just to be extra careful, don't assign if we're transitioning out.
        # (though theoretically that would be ok).
        if not self.is_transitioning_out() and player:
            player.assign_input_call(
                ('jumpPress', 'punchPress', 'bombPress', 'pickUpPress'),
                self._player_press)

    def _handle_server_restarts(self) -> bool:
        """Handle automatic restarts/shutdowns in server mode.

        Returns True if an action was taken; otherwise default action
        should occur (starting next round, etc).
        """
        # pylint: disable=cyclic-import

        # FIXME: Move server stuff to its own module.
        if self._allow_server_restart and _ba.app.server_config_dirty:
            from ba import _server
            from ba._lang import Lstr
            from ba._general import Call
            from ba._enums import TimeType
            if _ba.app.server_config.get('quit', False):
                if not self._kicked_off_server_shutdown:
                    if _ba.app.server_config.get(
                            'quit_reason') == 'restarting':
                        # FIXME: Should add a server-screen-message call
                        #  or something.
                        _ba.chat_message(
                            Lstr(resource='internal.serverRestartingText').
                            evaluate())
                        print(('Exiting for server-restart at ' +
                               time.strftime('%c')))
                    else:
                        print(('Exiting for server-shutdown at ' +
                               time.strftime('%c')))
                    with _ba.Context('ui'):
                        _ba.timer(2.0, _ba.quit, timetype=TimeType.REAL)
                    self._kicked_off_server_shutdown = True
                    return True
            else:
                if not self._kicked_off_server_restart:
                    print(('Running updated server config at ' +
                           time.strftime('%c')))
                    with _ba.Context('ui'):
                        _ba.timer(1.0,
                                  Call(_ba.pushcall,
                                       _server.launch_server_session),
                                  timetype=TimeType.REAL)
                    self._kicked_off_server_restart = True
                    return True
        return False