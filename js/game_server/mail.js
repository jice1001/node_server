/*
*	描述：邮件系统
*	作者：张亚磊
*	时间：2016/02/24
*/

function Mail() {
	this.game_player = null;
	this.mail_info = new Mail_Info();
}

Mail.prototype.load_data = function(game_player, player_data) {
	this.game_player = game_player;
	this.mail_info = player_data.mail_info;
}
	
Mail.prototype.save_data = function(player_data) {
	player_data.mail_info = this.mail_info;
}

Mail.prototype.fetch_mail = function() {
    if (typeof this.game_player.msg.mail_list == "object") {
        this.game_player.msg.mail_list.length = 0;
    } else {
        this.game_player.msg.mail_list = new Array();
    }

    for (var value of this.mail_info.mail_map.values()) {
  	    this.game_player.msg.mail_list.push(value);
    }
    this.game_player.send_success_msg(Msg.RES_MAIL_INFO);
}

Mail.prototype.pickup_mail = function(msg) {
    if (typeof this.game_player.msg.mail_id_list == "object") {
        this.game_player.msg.mail_id_list.length = 0;
    } else {
        this.game_player.msg.mail_id_list = new Array();
    }

	if (msg.mail_id == 0) {
		this.mail_info.mail_map.forEach(function(value, key, map) {
			var result = this.pickup_item_money(value);
			if (result == 0) {
			    this.game_player.msg.mail_id_list.push(key);
			}
    	});
	} else {
		var mail_detail = this.mail_info.mail_map.get(msg.mail_id);
		if (mail_detail == null) {
			return this.game_player.send_error_msg(Error_Code.CLIENT_PARAM_ERROR);
		}
		
		var result = this.pickup_item_money(value);
		if (result == 0) {
		    this.game_player.msg.mail_id_list.push(msg.mail_id);
		}
	}
	this.game_player.send_success_msg(Msg.RES_PICKUP_MAIL);
}

Mail.prototype.delete_mail = function(msg) {
    if (typeof this.game_player.msg.mail_id_list == "object") {
        this.game_player.msg.mail_id_list.length = 0;
    } else {
        this.game_player.msg.mail_id_list = new Array();
    }

	if (msg.mail_id == 0) {
		this.mail_info.mail_map.forEach(function(value, key, map) {
			var result = this.pickup_item_money(value);
			if (result == 0) {
			    this.game_player.msg.mail_id_list.push(key);
			}
    	});
    	this.mail_info.mail_map.clear();
	} else {
		var mail_detail = this.mail_info.mail_map.get(msg.mail_id);
		if (mail_detail == null) {
			return this.game_player.send_error_msg(Error_Code.CLIENT_PARAM_ERROR);
		}
		
		var result = this.pickup_item_money(value);
		if (result == 0) {
		    this.game_player.msg.mail_id_list.push(msg.mail_id);
			this.mail_info.mail_map.delete(msg.mail_id);
		}
	}
	this.game_player.send_success_msg(Msg.RES_DEL_MAIL);
}

Mail.prototype.pickup_item_money = function(mail_detail) {	
	var result = this.game_player.bag.bag_insert_item(mail_detail.item_info);
	if (result != 0) {
		return result;
	}
	
	result = this.game_player.bag.bag_add_money(mail_detail.copper, mail_detail.gold);
	if (result != 0) {
		return result;
	}
	
	mail_detail.pickup = true;
	this.game_player.data_change = true;
	return 0;
}
