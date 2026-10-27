"""Welcome: ztOS's first-boot screens. A few pages of how the phone is used,
the apps it comes with, and the SDK; shown once, and kept in the app
list for later."""
import os
import ztos

app = ztos.App()
DONE = os.path.expanduser("~/.config/ztos/welcomed")


def onward(page_class):
    return lambda: app.push(page_class())


class Step(ztos.Page):
    """A page of the welcome: a few lines, and next (and back) along the bottom."""
    following = None

    def __init__(self, title, lines, following=None):
        super().__init__(title)
        for line in lines:
            self.text(line, size=17)
        self.following = following
        buttons = []
        if app.stack:
            buttons.append(ztos.RoundButton("back", "back", app.pop))
        if following is not None:
            buttons.append(ztos.RoundButton("check" if following is Done else "send", "next", onward(following)))
        self.bar(*buttons)


class Done(ztos.Page):
    def __init__(self):
        super().__init__("all set")
        self.text("That's it. Everything here is in the Welcome app if you want it again.", size=17)
        self.entry("start using ztOS", "back to Start", self.finish)
        self.bar(ztos.RoundButton("back", "back", app.pop), ztos.RoundButton("check", "done", self.finish))

    def finish(self):
        os.makedirs(os.path.dirname(DONE), exist_ok=True)
        open(DONE, "w").close()
        app.close()


class Developers(Step):
    def __init__(self):
        super().__init__("make apps", [
            "ztOS apps are written in Python and Qt with the ztOS SDK. It is already on this phone.",
            "On a computer, get the SDK from the ztOS repository, then:",
            "ztos-sdk new MyApp     a new app\nztos-sdk run            try it on the computer\nztos-sdk deploy         put it on this phone",
            "Apps you install that way can be uninstalled from the app list: hold one, then uninstall.",
        ], Done)


class Apps(Step):
    def __init__(self):
        super().__init__("your apps", [
            "These came with ztOS. Tap one to open it now, or find it later in the app list.",
        ], Developers)
        for title, detail, command in (
            ("Settings", "Wi-Fi, accent colour, sounds, haptics", "ztsettings"),
            ("Music", "streaming and offline listening", "/usr/local/lib/ztos/apps/com.zerone.music"),
            ("superscan", "the web", "superscan"),
            ("Terminal", "GNOME Terminal", "gnome-terminal"),
            ("ZrnDevTest", "what the hardware says", "zrndevtest"),
        ):
            self.entry(title, detail, lambda c=command: ztos.launch(c))


class Gestures(Step):
    def __init__(self):
        super().__init__("getting around", [
            "Back, along the bottom, goes back a step. Hold it to see the apps you have open: swipe one down to close it.",
            "Pull down from the very top for the action centre: notifications, Wi-Fi, brightness and the rest.",
            "When there is something to type, the keyboard slides up over the app. Back puts it away.",
        ], Apps)


class StartScreen(Step):
    def __init__(self):
        super().__init__("Start", [
            "Start is where your tiles live. Tap a tile to open its app.",
            "Hold a tile to move it, resize it or unpin it. Tap empty space when you're done.",
            "Swipe left on Start for every app, in a list. Hold an app there to pin it to Start, or to uninstall it.",
        ], Gestures)


class Hello(Step):
    def __init__(self):
        super().__init__("welcome", [
            "This is ztOS.",
            "A few pages on how it works, and you're set. You can skip them: Back leaves at any time.",
        ], StartScreen)


app.run(Hello())
