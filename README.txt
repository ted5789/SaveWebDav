如有錯誤可以用執行系統R鍵給他更多的記憶體

【使用】
- 手把：上下移動（自動捲動定位）、A 選擇、B 返回、+ 離開；也可直接用觸控點選、拖曳捲動
- 設定：使用者名稱（第一次會自動抓取使用者暱稱，可手動修改）、WebDAV 位址、帳號、密碼
- 共享資料夾：「+ 新增共享資料夾」進入瀏覽，進到想要的資料夾後選「選擇這個資料夾」；
  選幾個就顯示幾個(最多 20)，每個顯示名稱與位置，點它可移除。
- 上傳檔名：Save_使用者名稱_年月日_時分秒.7z，只保留你自己最新 5 份
- 下載覆蓋：選版本 -> 確認 -> 下載 -> 解壓 -> 按原路徑覆蓋
  （還原前會自動備份目前的檔案到 SD 卡 config/Save_WebDAV/before_restore.7z）

【免責聲明】
- 本程式為個人自製軟體，使用風險自負。作者不對存檔遺失、損壞、覆蓋錯誤，
  或任何因使用本程式造成的損失（包含帳號或裝置相關處置）負責。
- 還原功能會覆蓋 SD 卡上的檔案，使用前請自行另外備份重要資料。
- 本程式不包含、也不處理任何遊戲本體、金鑰或版權保護機制，僅備份使用者自己
  指定的 SD 卡資料夾。

【隱私與安全】
- 程式會讀取使用者暱稱，用於備份檔名，並存在 SD 卡的設定檔中。
- 備份只會上傳到你自己設定的 WebDAV 伺服器，不經過作者的任何伺服器。
- 程式不驗證 HTTPS 憑證，請只連線到你信任的伺服器。
- WebDAV 密碼以明文存在設定檔中，請勿在不受信任的裝置上使用。
- 備份檔裡會記錄共享資料夾清單，還原時會自動加入，請只還原自己上傳的備份。

【AI 輔助聲明】
- 本專案的程式碼與圖示（icon.jpg）由 AI 輔助生成。

【授權】
- 本專案以 MIT 授權釋出，詳見 LICENSE。

【第三方函式庫】
本程式使用下列開源函式庫，各自適用其授權條款：
- libnx (ISC)
- SDL2、SDL2_ttf (zlib)
- libcurl (curl license)
- libarchive (BSD 3-Clause)
- mbedTLS (Apache 2.0)
- zstd (BSD)
- xz / liblzma (public domain)
- bzip2 (BSD-style)
- zlib (zlib license)
- FreeType (FreeType License)

Portions of this software are copyright © The FreeType Project
(www.freetype.org). All rights reserved.
