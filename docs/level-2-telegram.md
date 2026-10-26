# Level 2: messages from Telegram (no server)

Send text from Telegram on your phone; it appears on the round screen the next
time someone walks up to the desk. The ESP32 talks to Telegram directly, so you
do not need a server.

## 1. Create a bot (about 5 minutes)

1. In Telegram, open **@BotFather** and send `/newbot`.
2. Pick a name and a username ending in `bot`.
3. BotFather replies with a **token** like `123456789:AAH...`. Keep it private:
   anyone with the token can control your bot.

## 2. Give the token to Desk Buddy

1. Join the `DeskBuddy-XXXX` hotspot and open `http://mod.config`.
2. Under **Time, weather & Telegram**, paste the token and press **Save token**.
3. The status shows the bot's username once it is accepted.

## 3. Pair your chat

Open your bot in Telegram and press **Start** (or send `/start`).
The first chat that does this becomes the only chat Desk Buddy listens to.
Messages from any other chat are ignored.

## 4. Use it

| Send | Desk Buddy |
|---|---|
| Any text | "New message" when someone comes close; pat the head (touch pad) to read it, pat again to close. You get "Seen on the screen." in Telegram |
| `/wave` | The figure waves |
| `/peek` | The figure peeks up |
| `/unpair` | Forgets this chat; the next `/start` pairs again |

To pair a different chat, press **Unpair chat** on the setup page or send
`/unpair`. To turn Telegram off, save an empty token.

## Notes

- The screen font includes Latin, Cyrillic and the common Chinese, Japanese and
  Korean characters used by the interface. Characters outside it are skipped;
  you can add more by regenerating the font (see `firmware/tools/README.md`).
- Telegram is polled over HTTPS every 25 seconds (long polling), so messages
  usually arrive within a second or two.
- The token is stored on the ESP32 only. It never leaves the device except in
  requests to `api.telegram.org`.
