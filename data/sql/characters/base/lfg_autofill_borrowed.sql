-- Bots currently on loan to a party, and the level to put them back on.
--
-- This is the one piece of module state that has to outlive the process. A borrowed bot has
-- been re-levelled by PlayerbotFactory and nothing else on the server knows it was ever any
-- other level, so if the worldserver stops while a party is running, an in-memory-only record
-- would strand that bot at the party's level permanently. On startup the module reads this
-- table and queues every row for return before the bots can be recruited again.
--
-- Rows are short-lived by design: one appears when a bot is borrowed and is deleted the moment
-- it is handed back. A populated table on a running server means a dungeon run is in progress.

CREATE TABLE IF NOT EXISTS `lfg_autofill_borrowed`
(
    `guid`           INT UNSIGNED     NOT NULL COMMENT 'characters.guid of the borrowed bot',
    `original_level` TINYINT UNSIGNED NOT NULL COMMENT 'level to restore the bot to',
    PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;
