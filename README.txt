【怎麼拿到 Save_WebDAV.nro】
1. 登入 github.com，建立新的 repository。
2. 把本資料夾「裡面所有內容」(Makefile、source、.github) 拖進去上傳並 Commit。
   (若 .github 沒被上傳：Add file > Create new file，檔名輸入 .github/workflows/build.yml，貼上同名檔案內容)
3. Actions 分頁等 Build NRO 變綠勾，點進去在 Artifacts 下載 Save_WebDAV，解壓得到 .nro。
4. 放到 Switch SD 卡 /switch/ 執行。失敗請把紅色錯誤貼給我。

【使用】
- 手把：上下移動（自動捲動定位）、A 選擇、B 返回、+ 離開；也可直接用觸控點選、拖曳捲動
- 設定：使用者名稱（第一次會自動抓 Switch 帳號暱稱，可手動修改）、WebDAV 位址、帳號、密碼
- 共享資料夾：「+ 新增共享資料夾」進入瀏覽，進到想要的資料夾後選「選擇這個資料夾」；
  選幾個就顯示幾個(最多 20)，每個顯示名稱與位置，點它可移除。
- 所有設定與資料夾自動記住，存在 sdmc:/config/Save_WebDAV/config.txt（密碼為明文）
- 上傳檔名：Save_使用者名稱_年月日.7z（同一天再上傳會覆蓋當天那份），只保留你自己最新 5 份
- 下載覆蓋：選版本 -> 確認 -> 下載 -> 解壓 -> 按原路徑覆蓋
