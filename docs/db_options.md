# DB options table

Following table contains list of all options application uses. All options are stored in options database table, this table is shared between client and mailbox service in case they are using the same database. All client related options start with `client_` and mailbox related strat with `mailbox_`.

Other options may be added as needed in the future.

| Key                          | Type |
|------------------------------|:----:|
| client_nickname              | Text |
| client_onion_address         | Text |
| client_onion_public_key      | Bin  |
| client_onion_private_key     | Bin  |
| client_mailbox_id            | Bin  |
| client_mailbox_onion_address | Bin  |
| mailbox_onion_address        | Text |
| mailbox_onion_public_key     | Bin  |
| mailbox_onion_private_key    | Bin  |