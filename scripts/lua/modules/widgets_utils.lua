--
-- (C) 2020 - ntop.org
--

local widgets_utils = {}

require ("lua_utils")
local json = require("dkjson")
local rest_utils = require "rest_utils"
local datasources_utils = require("datasources_utils")
local datasource_keys = require "datasource_keys"

-- Widget Element Model:
-- name: the widget name
-- key: the string which identify the widget
-- type: the default type to render a widget

local REDIS_BASE_KEY = "ntopng.widgets"

local WIDGET_TYPES = {
    pie = {
        i18n = "Pie"
    },
    donut = {
        i18n = "Donut"
    },
    table = {
        i18n = "Table"
    },
    multibar = {
        i18n = "Multibar"
    }
}

local function create_hash_widget(name, ds_hash)
    return ntop.md5(name .. ds_hash)
end

local function check_widget_params(name, ds_hash, widget_type, params)

    if (isEmptyString(name)) then
        return false, "The widget name cannot be empty!"
    end

    if (isEmptyString(ds_hash)) then
        return false, "The associated datasource hash cannot be empty!"
    end

    -- Check if the passed ds hash is valid
    if (not datasources_utils.is_hash_valid(ds_hash)) then
        return false, "The passed hash is not valid!"
    end

    if (isEmptyString(widget_type) or WIDGET_TYPES[widget_type] == nil) then
        return false, "The passed widget type is not valid!"
    end

    if (params == nil) then
        return false, "The params are not valid!"
    end

    return true, nil
end

function widgets_utils.get_widget_types()
    return WIDGET_TYPES
end

-------------------------------------------------------------------------------
-- Create a new widget and save it to redis.

-- @return The function returns `true` if the passed
--         arguments meet the precondition, otherwise `false`
-------------------------------------------------------------------------------
function widgets_utils.add_widget(name, ds_hash, widget_type, params)
    local args_are_valid, message = check_widget_params(name, ds_hash, widget_type, params)

    if (not args_are_valid) then
        return args_are_valid, message
    end

    local key_widget = create_hash_widget(name, ds_hash)

    local new_widget = {
        key = key_widget,
        name = name,
        ds_hash = ds_hash,
        params = params,
        type = widget_type
    }

    ntop.setHashCache(REDIS_BASE_KEY, key_widget, json.encode(new_widget))

    return true, key_widget
end

-------------------------------------------------------------------------------
-- Edit the data source
-- @return True if the edit was successful, false otherwise
-------------------------------------------------------------------------------
function widgets_utils.edit_widget(widget_key, name, ds_hash, widget_type, params)

    local args_are_valid, message = check_widget_params(name, ds_hash, widget_type, params)
    if (not args_are_valid) then
        return args_are_valid, message
    end

    local json_widget = ntop.getHashCache(REDIS_BASE_KEY, widget_key)

    if (isEmptyString(json_widget)) then
        return false, "The widget was not found!"
    end

    local widget = json.decode(json_widget)
    widget.name = name
    widget.ds_hash = ds_hash
    widget.type = widget_type
    widget.params = params

    ntop.delHashCache(REDIS_BASE_KEY, widget_key)
    ntop.setHashCache(REDIS_BASE_KEY, widget_key, json.encode(widget))

    return true, widget_key
end

-------------------------------------------------------------------------------
-- Delete the widget from redis
-- @param widget_key The key of the widget to be removed (not nil)
-- @return True if the delete was successful, false otherwise
-------------------------------------------------------------------------------
function widgets_utils.delete_widget(widget_key)

    if (isEmptyString(widget_key)) then
        return false, "The widget key cannot be empty!"
    end

    if (isEmptyString(ntop.getHashCache(REDIS_BASE_KEY, widget_key))) then
        return false, "The widget to be removed was not found!"
    end

    ntop.delHashCache(REDIS_BASE_KEY, widget_key)

    return true, widget_key
end

-------------------------------------------------------------------------------
-- Get all widgets stored in redis
-- @return An array of widgets stored in redis, if no any widgets was found
-- it returns an empty array
-------------------------------------------------------------------------------
function widgets_utils.get_all_widgets()

    local widgets = ntop.getHashAllCache(REDIS_BASE_KEY)
    if (widgets == nil) then
        return {}
    end

    local all_widgets = {}

    for _, json_source in pairs(widgets) do
        all_widgets[#all_widgets + 1] = json.decode(json_source)
    end

    return all_widgets
end

-------------------------------------------------------------------------------
-- Get a widget stored in redis
-- @return The searched widget if the key is valid, otherwise a nil value
-------------------------------------------------------------------------------
function widgets_utils.get_widget(widget_key)

    local widget = ntop.getHashCache(REDIS_BASE_KEY, widget_key)
    if isEmptyString(widget) then return nil end

    return json.decode(widget)
end

-------------------------------------------------------------------------------
-- Answer to a widget request
-- @param widget Is a widget defined above
-- @param params Is a table which contains overriding params.
--               Example: {ifid, key, metric, begin_time, end_time, schema }
-------------------------------------------------------------------------------
function widgets_utils.generate_response(widget, params)
   local ds = datasources_utils.get(widget.ds_hash)
   local dirs = ntop.getDirs()
   package.path = dirs.installdir .. "/scripts/lua/datasources/?.lua;" .. package.path

   -- Remove trailer .lua from the origin
   local origin = ds.origin:gsub("%.lua", "")

   -- io.write("Executing "..origin..".lua\n")
   --tprint(widget)

   -- Call the origin to return
   local response = require(origin)

   if((response == nil) or (response == true)) then
      response = "{}"
   else
      response = response:getData(widget.type)
   end

   return json.encode({
	 widgetName = widget.name,
	 widgetType = widget.type,
	 dsRetention = ds.data_retention * 1000, -- msec
	 success = true,
	 data = response
   })
end

-- @brief Generate a rest response for the widget, by requesting data from multiple datasources and filtering it
function widgets_utils.rest_response()
   if not _POST or table.len(_POST) == 0 then
      rest_utils.answer(rest_utils.consts.err.invalid_args)
      return
   end

   -- Missing transformation
   if not _POST["transformation"] then
      rest_utils.answer(rest_utils.consts.err.widgets_missing_transformation)
      return
   end

   -- Check for datasources
   if not _POST["datasources"] or table.len(_POST["datasources"]) == 0 then
      rest_utils.answer(rest_utils.consts.err.widgets_missing_datasources)
      return
   end

   local datasources_data = {}
   for _, datasource in pairs(_POST["datasources"]) do
      -- Check if the datasource is valid and existing
      if not datasource.ds_type then
	 rest_utils.answer(rest_utils.consts.err.widgets_missing_datasource_type)
	 return
      end

      if not datasource_keys[datasource.ds_type] then
	 rest_utils.answer(rest_utils.consts.err.widgets_unknown_datasource_type)
	 return
      end

      -- Get the actual datasource key
      local datasource_key = datasource_keys[datasource.ds_type]
      -- Fetch the datasource class using the key
      local datasource_type = datasources_utils.get_source_type_by_key(datasource_key)
      local datasource_instance = datasource_type:new()

      -- Parse params into the instance
      if not datasource_instance:read_params(datasource.params) then
	 rest_utils.answer(rest_utils.consts.err.widgets_missing_datasource_params)
	 return
      end

      -- Fetch according to datasource parameters received via REST
      datasource_instance:fetch()

      -- Transform the data according to the requested transformation
      -- and set the transformed data as result
      local transformed_data = datasource_instance:transform(_POST["transformation"])

      datasources_data[#datasources_data + 1] = {
	 datasource = datasource,
	 metadata = datasource_instance:get_metadata(),
	 data = transformed_data
      }
   end
   
   rest_utils.answer(rest_utils.consts.success.ok, datasources_data)
end

return widgets_utils
