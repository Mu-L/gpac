/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2005-2024
 *			All rights reserved
 *
 *  This file is part of GPAC / common tools sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/xml.h>
#include <gpac/utf.h>

#ifndef GPAC_DISABLE_ZLIB
/*since 0.2.2, we use zlib for xmt/x3d reading to handle gz files*/
#include <zlib.h>

#if (defined(WIN32) || defined(_WIN32_WCE)) && !defined(__GNUC__)
#pragma comment(lib, "zlib")
#endif
#else
#define NO_GZIP
#endif


#define XML_INPUT_SIZE	4096

static u32 XML_MAX_CONTENT_SIZE = 0;


static GF_Err gf_xml_sax_parse_intern(GF_SAXParser *parser, char *current);

GF_STATIC char *xml_translate_xml_string(char *str)
{
	char *value;
	u32 size, i, j;
	if (!str || !strlen(str)) return NULL;
	value = (char *)gf_malloc(sizeof(char) * 500);
	size = 500;
	i = j = 0;
	while (str[i]) {
		if (j+20 >= size) {
			size += 500;
			value = (char *)gf_realloc(value, sizeof(char)*size);
		}
		if (str[i] == '&') {
			if (str[i+1]=='#') {
				char szChar[20], *end;
				u16 wchar[2];
				u32 val=0, _len;
				const unsigned short *srcp;
				strncpy(szChar, str+i, 10);
				szChar[10] = 0;
				end = strchr(szChar, ';');
				if (!end) break;
				end[1] = 0;
				i += (u32) strlen(szChar);
				wchar[1] = 0;
				if (szChar[2]=='x')
					sscanf(szChar, "&#x%x;", &val);
				else
					sscanf(szChar, "&#%u;", &val);
				wchar[0] = val;
				srcp = wchar;
				_len = gf_utf8_wcstombs(&value[j], 20, &srcp);
				if (_len == GF_UTF8_FAIL) _len = 0;
				j += _len;
			}
			else if (!strnicmp(&str[i], "&amp;", sizeof(char)*5)) {
				value[j] = '&';
				j++;
				i+= 5;
			}
			else if (!strnicmp(&str[i], "&lt;", sizeof(char)*4)) {
				value[j] = '<';
				j++;
				i+= 4;
			}
			else if (!strnicmp(&str[i], "&gt;", sizeof(char)*4)) {
				value[j] = '>';
				j++;
				i+= 4;
			}
			else if (!strnicmp(&str[i], "&apos;", sizeof(char)*6)) {
				value[j] = '\'';
				j++;
				i+= 6;
			}
			else if (!strnicmp(&str[i], "&quot;", sizeof(char)*6)) {
				value[j] = '\"';
				j++;
				i+= 6;
			} else {
				value[j] = str[i];
				j++;
				i++;
			}
		} else {
			value[j] = str[i];
			j++;
			i++;
		}
	}
	value[j] = 0;
	return value;
}


enum
{
	SAX_STATE_ATT_NAME,
	SAX_STATE_ATT_VALUE,
	SAX_STATE_ELEMENT,
	SAX_STATE_COMMENT,
	SAX_STATE_TEXT_CONTENT,
	SAX_STATE_ENTITY,
	SAX_STATE_SKIP_DOCTYPE,
	SAX_STATE_CDATA,
	SAX_STATE_DONE,
	SAX_STATE_XML_PROC,
	SAX_STATE_SYNTAX_ERROR,
	SAX_STATE_ALLOC_ERROR,
};

typedef struct
{
	u32 name_start, name_end;
	u32 val_start, val_end;
	Bool has_entities;
} GF_XMLSaxAttribute;


/* #define NO_GZIP */


struct _tag_sax_parser
{
	/*0: UTF-8, 1: UTF-16 BE, 2: UTF-16 LE. String input is always converted back to utf8*/
	s32 unicode_type;
	char *buffer;
	/*alloc size, line size and current position*/
	u32 alloc_size, line_size, current_pos;
	/*current node depth*/
	u32 node_depth;

	/*gz input file*/
#ifdef NO_GZIP
	FILE *f_in;
#else
	gzFile gz_in;
#endif
	/*current line , file size and pos for user notif*/
	u32 line, file_size, file_pos;

	/*SAX callbacks*/
	gf_xml_sax_node_start sax_node_start;
	gf_xml_sax_node_end sax_node_end;
	gf_xml_sax_text_content sax_text_content;
	void *sax_cbck;
	gf_xml_sax_progress on_progress;

	u32 sax_state;
	u32 init_state;
	GF_List *entities;
	char att_sep;
	Bool in_entity, suspended;
	u32 in_quote;

	u32 elt_start_pos, elt_end_pos;

	/*last error found*/
	char err_msg[1000];

	u32 att_name_start, elt_name_start, elt_name_end, text_start, text_end;
	u32 text_check_escapes;

	GF_XMLAttribute *attrs;
	GF_XMLSaxAttribute *sax_attrs;
	u32 nb_attrs, nb_alloc_attrs;
	u32 ent_rec_level;
};

static GF_XMLSaxAttribute *xml_get_sax_attribute(GF_SAXParser *parser)
{
	if (parser->nb_attrs==parser->nb_alloc_attrs) {
		parser->nb_alloc_attrs++;
		parser->sax_attrs = (GF_XMLSaxAttribute *)gf_realloc(parser->sax_attrs, sizeof(GF_XMLSaxAttribute)*parser->nb_alloc_attrs);
		parser->attrs = (GF_XMLAttribute *)gf_realloc(parser->attrs, sizeof(GF_XMLAttribute)*parser->nb_alloc_attrs);
	}
	return &parser->sax_attrs[parser->nb_attrs++];
}

static void xml_sax_swap(GF_SAXParser *parser)
{
	if (parser->current_pos && ((parser->sax_state==SAX_STATE_TEXT_CONTENT) || (parser->sax_state==SAX_STATE_COMMENT) ) ) {
		if (parser->line_size >= parser->current_pos) {
			parser->line_size -= parser->current_pos;
			parser->file_pos += parser->current_pos;
			if (parser->line_size) memmove(parser->buffer, parser->buffer + parser->current_pos, sizeof(char)*parser->line_size);
			parser->buffer[parser->line_size] = 0;
			parser->current_pos = 0;
		}
	}
}

static void format_sax_error(GF_SAXParser *parser, u32 linepos, const char* fmt, ...)
{
	va_list args;
	u32 len;

	if (!parser) return;

	va_start(args, fmt);
	vsnprintf(parser->err_msg, GF_ARRAY_LENGTH(parser->err_msg), fmt, args);
	va_end(args);

	if (strlen(parser->err_msg)+30 < GF_ARRAY_LENGTH(parser->err_msg)) {
		char szM[20];
		snprintf(szM, 20, " - Line %d: ", parser->line + 1);
		strcat(parser->err_msg, szM);
		len = (u32) strlen(parser->err_msg);
		strncpy(parser->err_msg + len, parser->buffer+ (linepos ? linepos : parser->current_pos), 10);
		parser->err_msg[len + 10] = 0;
	}
	parser->sax_state = SAX_STATE_SYNTAX_ERROR;
}

static void xml_sax_node_end(GF_SAXParser *parser, Bool had_children)
{
	char *name, c;

	gf_assert(parser->elt_name_start);
	gf_assert(parser->elt_name_end);
	if (!parser->node_depth) {
		format_sax_error(parser, 0, "Markup error");
		return;
	}
	c = parser->buffer[parser->elt_name_end - 1];
	parser->buffer[parser->elt_name_end - 1] = 0;
	name = parser->buffer + parser->elt_name_start - 1;

	if (parser->sax_node_end) {
		char *sep = strchr(name, ':');
		if (sep) {
			sep[0] = 0;
			parser->sax_node_end(parser->sax_cbck, sep+1, name);
			sep[0] = ':';
		} else {
			parser->sax_node_end(parser->sax_cbck, name, NULL);
		}
	}
	parser->buffer[parser->elt_name_end - 1] = c;
	parser->node_depth--;
	if (!parser->init_state && !parser->node_depth && parser->sax_state<SAX_STATE_SYNTAX_ERROR) parser->sax_state = SAX_STATE_DONE;
	xml_sax_swap(parser);
	parser->text_start = parser->text_end = 0;
}

static void xml_sax_node_start(GF_SAXParser *parser)
{
	Bool has_entities = GF_FALSE;
	u32 i;
	char c, *name;

	gf_assert(parser->elt_name_start && parser->elt_name_end);
	c = parser->buffer[parser->elt_name_end - 1];
	parser->buffer[parser->elt_name_end - 1] = 0;
	name = parser->buffer + parser->elt_name_start - 1;

	for (i=0; i<parser->nb_attrs; i++) {
		parser->attrs[i].name = parser->buffer + parser->sax_attrs[i].name_start - 1;
		parser->buffer[parser->sax_attrs[i].name_end-1] = 0;
		parser->attrs[i].value = parser->buffer + parser->sax_attrs[i].val_start - 1;
		parser->buffer[parser->sax_attrs[i].val_end-1] = 0;

		if (strchr(parser->attrs[i].value, '&')) {
			parser->sax_attrs[i].has_entities = GF_TRUE;
			has_entities = GF_TRUE;
			parser->attrs[i].value = xml_translate_xml_string(parser->attrs[i].value);
		}
		/*store first char pos after current attrib for node peeking*/
		parser->att_name_start = parser->sax_attrs[i].val_end;
	}

	if (parser->sax_node_start) {
		char *sep = strchr(name, ':');
		if (sep) {
			sep[0] = 0;
			parser->sax_node_start(parser->sax_cbck, sep+1, name, parser->attrs, parser->nb_attrs);
			sep[0] = ':';
		} else {
			parser->sax_node_start(parser->sax_cbck, name, NULL, parser->attrs, parser->nb_attrs);
		}
	}
	parser->att_name_start = 0;
	parser->buffer[parser->elt_name_end - 1] = c;
	parser->node_depth++;
	if (has_entities) {
		for (i=0; i<parser->nb_attrs; i++) {
			if (parser->sax_attrs[i].has_entities) {
				parser->sax_attrs[i].has_entities = GF_FALSE;
				gf_free(parser->attrs[i].value);
			}
		}
	}
	parser->nb_attrs = 0;
	xml_sax_swap(parser);
	parser->text_start = parser->text_end = 0;
}

static Bool xml_sax_parse_attribute(GF_SAXParser *parser)
{
	char *sep;
	GF_XMLSaxAttribute *att = NULL;

	/*looking for attribute name*/
	if (parser->sax_state==SAX_STATE_ATT_NAME) {
		/*looking for start*/
		if (!parser->att_name_start) {
			while (parser->current_pos < parser->line_size) {
				u8 c = parser->buffer[parser->current_pos];
				switch (c) {
				case '\n':
					parser->line++;
				case ' ':
				case '\r':
				case '\t':
					parser->current_pos++;
					continue;
				/*end of element*/
				case '?':
					if (parser->init_state!=1) break;
				case '/':
					/*not enough data*/
					if (parser->current_pos+1 == parser->line_size) return GF_TRUE;
					if (parser->buffer[parser->current_pos+1]=='>') {
						parser->current_pos+=2;
						parser->elt_end_pos = parser->file_pos + parser->current_pos - 1;
						/*done parsing attr AND elements*/
						if (!parser->init_state) {
							xml_sax_node_start(parser);
							/*move to SAX_STATE_TEXT_CONTENT to force text flush*/
							parser->sax_state = SAX_STATE_TEXT_CONTENT;
							xml_sax_node_end(parser, GF_FALSE);
						} else {
							parser->nb_attrs = 0;
						}
						parser->sax_state = (parser->init_state) ? SAX_STATE_ELEMENT : SAX_STATE_TEXT_CONTENT;
						parser->text_start = parser->text_end = 0;
						return GF_FALSE;
					}
					if (!parser->in_quote && (c=='/')) {
						if (!parser->init_state) {
							format_sax_error(parser, 0, "Markup error");
							return GF_TRUE;
						}
					}
					break;
				case '"':
					if (parser->sax_state==SAX_STATE_ATT_VALUE) break;
					if (parser->in_quote && (parser->in_quote!=c) ) {
						format_sax_error(parser, 0, "Markup error");
						return GF_TRUE;
					}
					if (parser->in_quote) parser->in_quote = 0;
					else parser->in_quote = c;
					break;
				case '>':
					parser->current_pos+=1;
					/*end of <!DOCTYPE>*/
					if (parser->init_state) {
						if (parser->init_state==1) {
							format_sax_error(parser, 0, "Invalid <!DOCTYPE...> or <?xml...?>");
							return GF_TRUE;
						}
						parser->sax_state = SAX_STATE_ELEMENT;
						return GF_FALSE;
					}
					/*done parsing attr*/
					parser->sax_state = SAX_STATE_TEXT_CONTENT;
					xml_sax_node_start(parser);
					return GF_FALSE;
				case '[':
					if (parser->init_state) {
						parser->current_pos+=1;
						if (parser->init_state==1) {
							format_sax_error(parser, 0, "Invalid <!DOCTYPE...> or <?xml...?>");
							return GF_TRUE;
						}
						parser->sax_state = SAX_STATE_ELEMENT;
						return GF_FALSE;
					}
					break;
				case '<':
					format_sax_error(parser, 0, "Invalid character '<'");
					return GF_FALSE;
				/*first char of attr name*/
				default:
					parser->att_name_start = parser->current_pos + 1;
					break;
				}
				parser->current_pos++;
				if (parser->att_name_start) break;
			}
			if (parser->current_pos == parser->line_size) return GF_TRUE;
		}

		if (parser->init_state==2) {
			sep = strchr(parser->buffer + parser->att_name_start - 1, parser->in_quote ?  parser->in_quote : ' ');
			/*not enough data*/
			if (!sep) return GF_TRUE;
			parser->current_pos = (u32) (sep - parser->buffer);
			parser->att_name_start = 0;
			if (parser->in_quote) {
				parser->current_pos++;
				parser->in_quote = 0;
			}
			return GF_FALSE;
		}

		/*looking for '"'*/
		if (parser->att_name_start) {
			u32 i, first=1;
			sep = strchr(parser->buffer + parser->att_name_start - 1, '=');
			/*not enough data*/
			if (!sep) return GF_TRUE;

			parser->current_pos = (u32) (sep - parser->buffer);
			att = xml_get_sax_attribute(parser);
			att->name_start = parser->att_name_start;
			att->name_end = parser->current_pos + 1;
			while (strchr(" \n\t", parser->buffer[att->name_end - 2])) {
				gf_assert(att->name_end);
				att->name_end --;
			}
			att->has_entities = GF_FALSE;

			for (i=att->name_start; i<att->name_end; i++) {
				char c = parser->buffer[i-1];
				if ((c>='a') && (c<='z')) {}
				else if ((c>='A') && (c<='Z')) {}
				else if ((c==':') || (c=='_')) {}

				else if (!first && ((c=='-') || (c=='.') || ((c>='0') && (c<='9')) )) {}

				else {
					format_sax_error(parser, att->name_start-1, "Invalid character \'%c\' for attribute name", c);
					return GF_TRUE;
				}

				first=0;
			}

			parser->att_name_start = 0;
			parser->current_pos++;
			parser->sax_state = SAX_STATE_ATT_VALUE;

		}
	}

	if (parser->sax_state == SAX_STATE_ATT_VALUE) {
		att = &parser->sax_attrs[parser->nb_attrs-1];
		/*looking for first delimiter*/
		if (!parser->att_sep) {
			while (parser->current_pos < parser->line_size) {
				u8 c = parser->buffer[parser->current_pos];
				switch (c) {
				case '\n':
					parser->line++;
				case ' ':
				case '\r':
				case '\t':
					parser->current_pos++;
					continue;
				case '\'':
				case '"':
					parser->att_sep = c;
					att->val_start = parser->current_pos + 2;
					break;
				default:
					// garbage char before value separator -> error
					goto att_retry;
				}
				parser->current_pos++;
				if (parser->att_sep) break;
			}
			if (parser->current_pos == parser->line_size) return GF_TRUE;
		}

att_retry:

		if (!parser->att_sep) {
			format_sax_error(parser, parser->current_pos, "Invalid character %c before attribute value separator", parser->buffer[parser->current_pos]);
			return GF_TRUE;
		}
		sep = strchr(parser->buffer + parser->current_pos, parser->att_sep);
		if (!sep || !sep[1]) return GF_TRUE;

		if (sep[1]==parser->att_sep) {
			format_sax_error(parser, (u32) (sep - parser->buffer), "Invalid character %c after attribute value separator %c ", sep[1], parser->att_sep);
			return GF_TRUE;
		}

		if (!parser->init_state && (strchr(" />\n\t\r", sep[1])==NULL)) {
			parser->current_pos = (u32) (sep - parser->buffer + 1);
			goto att_retry;
		}

		parser->current_pos = (u32) (sep - parser->buffer);
		att->val_end = parser->current_pos + 1;
		parser->current_pos++;

		/*"style" always at the beginning of the attributes for ease of parsing*/
		if (!strncmp(parser->buffer + att->name_start-1, "style", 5)) {
			GF_XMLSaxAttribute prev = parser->sax_attrs[0];
			parser->sax_attrs[0] = *att;
			*att = prev;
		}
		parser->att_sep = 0;
		parser->sax_state = SAX_STATE_ATT_NAME;
		parser->att_name_start = 0;
		return GF_FALSE;
	}
	return GF_TRUE;
}


typedef struct
{
	char *name;
	char *value;
	u32 namelen;
	u8 sep;
} XML_Entity;

static void xml_sax_flush_text(GF_SAXParser *parser)
{
	char *text, c;
	if (!parser->text_start || parser->init_state || !parser->sax_text_content) return;

	gf_assert(parser->text_start < parser->text_end);

	c = parser->buffer[parser->text_end-1];
	parser->buffer[parser->text_end-1] = 0;
	text = parser->buffer + parser->text_start-1;

	/*solve XML built-in entities*/
//old code commented for ref, we now track escape chars
//	if (strchr(text, '&') && strchr(text, ';')) {
	if (parser->text_check_escapes==0x3) {
		char *xml_text = xml_translate_xml_string(text);
		if (xml_text) {
			parser->sax_text_content(parser->sax_cbck, xml_text, (parser->sax_state==SAX_STATE_CDATA) ? GF_TRUE : GF_FALSE);
			gf_free(xml_text);
		}
	} else {
		parser->sax_text_content(parser->sax_cbck, text, (parser->sax_state==SAX_STATE_CDATA) ? GF_TRUE : GF_FALSE);
	}
	parser->buffer[parser->text_end-1] = c;
	parser->text_start = parser->text_end = 0;
	parser->text_check_escapes = 0;
}

static void xml_sax_store_text(GF_SAXParser *parser, u32 txt_len)
{
	if (!txt_len) return;

	if (!parser->text_start) {
		parser->text_check_escapes = 0;
		parser->text_start = parser->current_pos + 1;
		parser->text_end = parser->text_start + txt_len;
		parser->current_pos += txt_len;
		gf_assert(parser->current_pos <= parser->line_size);
		return;
	}
	/*contiguous text*/
	if (parser->text_end && (parser->text_end-1 == parser->current_pos)) {
		parser->text_end += txt_len;
		parser->current_pos += txt_len;
		gf_assert(parser->current_pos <= parser->line_size);
		return;
	}
	/*need to flush*/
	xml_sax_flush_text(parser);

	parser->text_start = parser->current_pos + 1;
	parser->text_end = parser->text_start + txt_len;
	parser->current_pos += txt_len;
	gf_assert(parser->current_pos <= parser->line_size);
}

static char *xml_get_current_text(GF_SAXParser *parser)
{
	char *text, c;
	if (!parser->text_start) return NULL;

	c = parser->buffer[parser->text_end-1];
	parser->buffer[parser->text_end-1] = 0;
	text = gf_strdup(parser->buffer + parser->text_start-1);
	parser->buffer[parser->text_end-1] = c;
	parser->text_start = parser->text_end = 0;
	return text;
}

static void xml_sax_skip_doctype(GF_SAXParser *parser)
{
	while (parser->current_pos < parser->line_size) {
		if (parser->buffer[parser->current_pos]=='>') {
			parser->sax_state = SAX_STATE_ELEMENT;
			parser->current_pos++;
			xml_sax_swap(parser);
			return;
		}
		parser->current_pos++;
	}
}

static void xml_sax_skip_xml_proc(GF_SAXParser *parser)
{
	while (parser->current_pos < parser->line_size) {
		if ((parser->current_pos + 1 < parser->line_size) && (parser->buffer[parser->current_pos]=='?') && (parser->buffer[parser->current_pos+1]=='>')) {
			parser->sax_state = SAX_STATE_ELEMENT;
			parser->current_pos++;
			xml_sax_swap(parser);
			return;
		}
		parser->current_pos++;
	}
}


static void xml_sax_parse_entity(GF_SAXParser *parser)
{
	char szC[2];
	char *ent_name=NULL;
	u32 i = 0;
	XML_Entity *ent = (XML_Entity *)gf_list_last(parser->entities);
	char *skip_chars = " \t\n\r";
	i=0;
	if (ent && ent->value) ent = NULL;
	if (ent) skip_chars = NULL;
	szC[1]=0;

	while (parser->current_pos+i < parser->line_size) {
		u8 c = parser->buffer[parser->current_pos+i];
		if (skip_chars && strchr(skip_chars, c)) {
			if (c=='\n') parser->line++;
			parser->current_pos++;
			continue;
		}
		if (!ent && (c=='%')) {
			parser->current_pos+=i+1;
			parser->sax_state = SAX_STATE_SKIP_DOCTYPE;
			if (ent_name) gf_free(ent_name);
			return;
		}
		else if (!ent && ((c=='\"') || (c=='\'')) ) {
			GF_SAFEALLOC(ent, XML_Entity);
			if (!ent) {
				parser->sax_state = SAX_STATE_ALLOC_ERROR;
				if (ent_name) gf_free(ent_name);
				return;
			}
			if (!ent_name) gf_dynstrcat(&ent_name, "", NULL);

			ent->name = ent_name;
			ent_name=NULL;
			ent->namelen = (u32) strlen(ent->name);
			ent->sep = c;
			parser->current_pos += 1+i;
			gf_assert(parser->current_pos < parser->line_size);
			xml_sax_swap(parser);
			i=0;
			gf_list_add(parser->entities, ent);
			skip_chars = NULL;
		} else if (ent && c==ent->sep) {
			if (ent_name) gf_free(ent_name);
			xml_sax_store_text(parser, i);

			ent->value = xml_get_current_text(parser);
			if (!ent->value) ent->value = gf_strdup("");

			parser->current_pos += 1;
			gf_assert(parser->current_pos < parser->line_size);
			xml_sax_swap(parser);
			parser->sax_state = SAX_STATE_SKIP_DOCTYPE;
			return;
		} else if (!ent) {
			szC[0] = c;
			gf_dynstrcat(&ent_name, szC, NULL);
			i++;
		} else {
			i++;
		}
	}
	if (ent_name) gf_free(ent_name);
	if (ent && !ent->value)
		parser->sax_state = SAX_STATE_SYNTAX_ERROR;
	xml_sax_store_text(parser, i);
}

static void xml_sax_cdata(GF_SAXParser *parser)
{
	char *cd_end = strstr(parser->buffer + parser->current_pos, "]]>");
	if (!cd_end) {
		xml_sax_store_text(parser, parser->line_size - parser->current_pos);
	} else {
		u32 size = (u32) (cd_end - (parser->buffer + parser->current_pos));
		xml_sax_store_text(parser, size);
		xml_sax_flush_text(parser);
		parser->current_pos += 3;
		gf_assert(parser->current_pos <= parser->line_size);
		parser->sax_state = SAX_STATE_TEXT_CONTENT;
	}
}

static Bool xml_sax_parse_comments(GF_SAXParser *parser)
{
	char *end = strstr(parser->buffer + parser->current_pos, "-->");
	if (!end) {
		if (parser->line_size>3)
			parser->current_pos = parser->line_size-3;
		xml_sax_swap(parser);
		return GF_FALSE;
	}

	parser->current_pos += 3 + (u32) (end - (parser->buffer + parser->current_pos) );
	gf_assert(parser->current_pos <= parser->line_size);
	parser->sax_state = SAX_STATE_TEXT_CONTENT;
	parser->text_start = parser->text_end = 0;
	xml_sax_swap(parser);
	return GF_TRUE;
}



static GF_Err xml_sax_parse(GF_SAXParser *parser, Bool force_parse)
{
	u32 i = 0;
	Bool is_text;
	u32 is_end;
	u8 c;
	char *elt, sep;
	u32 cdata_sep;

	while (parser->current_pos<parser->line_size) {
		if (!force_parse && parser->suspended) goto exit;

restart:
		is_text = GF_FALSE;
		switch (parser->sax_state) {
		/*load an XML element*/
		case SAX_STATE_TEXT_CONTENT:
			is_text = GF_TRUE;
		case SAX_STATE_ELEMENT:
			elt = NULL;
			i=0;
			while ((c = parser->buffer[parser->current_pos+i]) !='<') {
				if ((parser->init_state==2) && (c ==']')) {
					parser->sax_state = SAX_STATE_ATT_NAME;
					parser->current_pos+=i+1;
					goto restart;
				}
				i++;
				if (c=='\n') parser->line++;
				if (is_text) {
					if (c=='&') parser->text_check_escapes |= 1;
					else if (c==';') parser->text_check_escapes |= 2;
				}

				if (parser->current_pos+i==parser->line_size) {
					if ((parser->line_size >= XML_MAX_CONTENT_SIZE) && !parser->init_state) {
						GF_LOG(GF_LOG_ERROR, GF_LOG_CORE, ("[XML] Content size larger than max allowed %u, try increasing limit using `-xml-max-csize`\n", XML_MAX_CONTENT_SIZE));
						parser->sax_state = SAX_STATE_SYNTAX_ERROR;
					}

					goto exit;
				}
			}
			if (is_text && i) {
				u32 has_esc = parser->text_check_escapes;
				xml_sax_store_text(parser, i);
				parser->text_check_escapes = has_esc;
				parser->sax_state = SAX_STATE_ELEMENT;
			} else if (i) {
				parser->current_pos += i;
				gf_assert(parser->current_pos < parser->line_size);
			}
			is_end = 0;
			i = 0;
			cdata_sep = 0;
			while (1) {
				c = parser->buffer[parser->current_pos+1+i];
				if (!strncmp(parser->buffer+parser->current_pos+1+i, "!--", 3)) {
					parser->sax_state = SAX_STATE_COMMENT;
					i += 3;
					break;
				}
				if (!c) {
					goto exit;
				}
				if ((c=='\t') || (c=='\r') || (c==' ') ) {
					if (i) break;
					else parser->current_pos++;
				}
				else if (c=='\n') {
					parser->line++;
					if (i) break;
					else parser->current_pos++;
				}
				else if (c=='>') break;
				else if (c=='=') break;
				else if (c=='[') {
					i++;
					if (!cdata_sep) cdata_sep = 1;
					else {
						break;
					}
				}
				else if (c=='/') {
					is_end = !i ? 1 : 2;
					i++;
				} else if (c=='<') {
					if (parser->sax_state != SAX_STATE_COMMENT) {
						parser->sax_state = SAX_STATE_SYNTAX_ERROR;
						return GF_CORRUPTED_DATA;
					}
				} else {
					i++;
				}
				/*				if ((c=='[') && (parser->buffer[parser->elt_name_start-1 + i-2]=='A') ) break; */
				if (parser->current_pos+1+i==parser->line_size) {
					goto exit;
				}
			}
			if (i) {
				parser->elt_name_start = parser->current_pos+1 + 1;
				if (is_end==1) parser->elt_name_start ++;
				if (is_end==2) parser->elt_name_end = parser->current_pos+1+i;
				else parser->elt_name_end = parser->current_pos+1+i + 1;
			}
			if (is_end) {
				xml_sax_flush_text(parser);
				parser->elt_end_pos = parser->file_pos + parser->current_pos + i;
				if (is_end==2) {
					parser->sax_state = SAX_STATE_ELEMENT;
					xml_sax_node_start(parser);
					xml_sax_node_end(parser, GF_FALSE);
				} else {
					parser->elt_end_pos += parser->elt_name_end - parser->elt_name_start;
					xml_sax_node_end(parser, GF_TRUE);
				}
				if (parser->sax_state == SAX_STATE_SYNTAX_ERROR) break;
				parser->current_pos+=2+i;
				parser->sax_state = SAX_STATE_TEXT_CONTENT;
				break;
			}
			if (!parser->elt_name_end) {
				return GF_CORRUPTED_DATA;
			}
			sep = parser->buffer[parser->elt_name_end-1];
			parser->buffer[parser->elt_name_end-1] = 0;
			elt = parser->buffer + parser->elt_name_start-1;

			parser->sax_state = SAX_STATE_ATT_NAME;
			gf_assert(parser->elt_start_pos <= parser->file_pos + parser->current_pos);
			parser->elt_start_pos = parser->file_pos + parser->current_pos;

			if (!strncmp(elt, "!--", 3)) {
				xml_sax_flush_text(parser);
				parser->sax_state = SAX_STATE_COMMENT;
				if (i>3) parser->current_pos -= (i-3);
			}
			else if (!strcmp(elt, "?xml")) parser->init_state = 1;
			else if (!strcmp(elt, "!DOCTYPE")) parser->init_state = 2;
			else if (!strcmp(elt, "!ENTITY")) parser->sax_state = SAX_STATE_ENTITY;
			else if (!strcmp(elt, "!ATTLIST") || !strcmp(elt, "!ELEMENT")) parser->sax_state = SAX_STATE_SKIP_DOCTYPE;
			else if (!strcmp(elt, "![CDATA["))
				parser->sax_state = SAX_STATE_CDATA;
			else if (elt[0]=='?') {
				i--;
				parser->sax_state = SAX_STATE_XML_PROC;
			}
			/*node found*/
			else {
				xml_sax_flush_text(parser);
				if (parser->init_state) {
					parser->init_state = 0;
					/*that's a bit ugly: since we solve entities when appending text, we need to
					reparse the current buffer*/
					if (gf_list_count(parser->entities)) {
						char *orig_buf;
						GF_Err e;
						parser->buffer[parser->elt_name_end-1] = sep;
						orig_buf = gf_strdup(parser->buffer + parser->current_pos);
						parser->current_pos = 0;
						parser->line_size = 0;
						parser->elt_start_pos = 0;
						parser->sax_state = SAX_STATE_TEXT_CONTENT;
						parser->ent_rec_level++;
						if (parser->ent_rec_level>100) {
							GF_LOG(GF_LOG_WARNING, GF_LOG_CORE, ("[XML] Too many recursions in entity solving, max 100 allowed\n"));
							e = GF_NOT_SUPPORTED;
						} else {
							e = gf_xml_sax_parse_intern(parser, orig_buf);
							parser->ent_rec_level--;
						}
						gf_free(orig_buf);
						return e;
					}
				}
			}
			parser->current_pos+=1+i;
			parser->buffer[parser->elt_name_end-1] = sep;
			break;
		case SAX_STATE_COMMENT:
			if (!xml_sax_parse_comments(parser)) {
				xml_sax_swap(parser);
				goto exit;
			}
			break;
		case SAX_STATE_ATT_NAME:
		case SAX_STATE_ATT_VALUE:
			if (xml_sax_parse_attribute(parser))
				goto exit;
			break;
		case SAX_STATE_ENTITY:
			xml_sax_parse_entity(parser);
			break;
		case SAX_STATE_SKIP_DOCTYPE:
			xml_sax_skip_doctype(parser);
			break;
		case SAX_STATE_XML_PROC:
			xml_sax_skip_xml_proc(parser);
			break;
		case SAX_STATE_CDATA:
			xml_sax_cdata(parser);
			break;
		case SAX_STATE_SYNTAX_ERROR:
			return GF_CORRUPTED_DATA;
		case SAX_STATE_ALLOC_ERROR:
			return GF_OUT_OF_MEM;
		case SAX_STATE_DONE:
			return GF_EOS;
		}
	}
exit:
#if 0
	if (is_text) {
		if (i) xml_sax_store_text(parser, i);
		/*DON'T FLUSH TEXT YET, wait for next '<' to do so otherwise we may corrupt xml base entities (&apos;, ...)*/
	}
#endif
	xml_sax_swap(parser);

	if (parser->sax_state==SAX_STATE_SYNTAX_ERROR)
		return GF_CORRUPTED_DATA;
	else
		return GF_OK;
}

static GF_Err xml_sax_append_string(GF_SAXParser *parser, char *string)
{
	u32 size = parser->line_size;
	u32 nl_size = string ? (u32) strlen(string) : 0;

	if (!nl_size) return GF_OK;

	if ( (parser->alloc_size < size+nl_size+1)
	        /*		|| (parser->alloc_size / 2 ) > size+nl_size+1 */
	   )
	{
		parser->alloc_size = size+nl_size+1;
		parser->alloc_size = 3 * parser->alloc_size / 2;
		parser->buffer = (char*)gf_realloc(parser->buffer, sizeof(char) * parser->alloc_size);
		if (!parser->buffer ) return GF_OUT_OF_MEM;
	}
	memcpy(parser->buffer+size, string, sizeof(char)*nl_size);
	parser->buffer[size+nl_size] = 0;
	parser->line_size = size+nl_size;
	return GF_OK;
}

static XML_Entity *gf_xml_locate_entity(GF_SAXParser *parser, char *ent_start, Bool *needs_text)
{
	u32 i, count;
	u32 len = (u32) strlen(ent_start);

	*needs_text = GF_FALSE;
	count = gf_list_count(parser->entities);

	for (i=0; i<count; i++) {
		XML_Entity *ent = (XML_Entity *)gf_list_get(parser->entities, i);
		if (len < ent->namelen + 1) {
			if (strncmp(ent->name, ent_start, len))
			 	return NULL;

			*needs_text = GF_TRUE;
			return NULL;
		}
		if (!strncmp(ent->name, ent_start, ent->namelen) && (ent_start[ent->namelen]==';')) {
			return ent;
		}
	}
	return NULL;
}


static GF_Err gf_xml_sax_parse_intern(GF_SAXParser *parser, char *current)
{
	u32 count;
	/*solve entities*/
	count = gf_list_count(parser->entities);
	while (count) {
		char *entityEnd;
		XML_Entity *ent;
		char *entityStart = strstr(current, "&");
		Bool needs_text;
		u32 line_num;

		/*if in entity, the start of the entity is in the buffer !!*/
		if (parser->in_entity) {
			u32 len;
			char *name;
			entityEnd = strstr(current, ";");
			if (!entityEnd) return xml_sax_append_string(parser, current);

			entityStart = strrchr(parser->buffer, '&');
			if (!entityStart) return xml_sax_append_string(parser, current);

			entityEnd[0] = 0;
			len = (u32) strlen(entityStart) + (u32) strlen(current) + 1;
			name = (char*)gf_malloc(sizeof(char)*len);
			sprintf(name, "%s%s;", entityStart+1, current);

			ent = gf_xml_locate_entity(parser, name, &needs_text);
			gf_free(name);

			//entity not found, parse as regular string
			if (!ent && !needs_text) {
				xml_sax_append_string(parser, current);
				xml_sax_parse(parser, GF_TRUE);
				entityEnd[0] = ';';
				current = entityEnd;
				parser->in_entity = GF_FALSE;
				continue;
			}
			if (!ent) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_PARSER, ("[SAX] Entity not found\n"));
				return GF_CORRUPTED_DATA;
			}
			/*truncate input buffer*/
			parser->line_size -= (u32) strlen(entityStart);
			entityStart[0] = 0;

			parser->in_entity = GF_FALSE;
			entityEnd[0] = ';';
			current = entityEnd+1;
		} else {
			if (!entityStart) break;

			ent = gf_xml_locate_entity(parser, entityStart+1, &needs_text);

			/*store current string before entity start*/
			entityStart[0] = 0;
			xml_sax_append_string(parser, current);
			xml_sax_parse(parser, GF_TRUE);
			entityStart[0] = '&';

			/*this is not an entitiy*/
			if (!ent && !needs_text) {
				xml_sax_append_string(parser, "&");
				current = entityStart+1;
				continue;
			}

			if (!ent) {
				parser->in_entity = GF_TRUE;
				/*store entity start*/
				return xml_sax_append_string(parser, entityStart);
			}
			current = entityStart + ent->namelen + 2;
		}
		/*append entity*/
		line_num = parser->line;
		xml_sax_append_string(parser, ent->value);
		GF_Err e = xml_sax_parse(parser, GF_TRUE);
		parser->line = line_num;
		if (e) return e;

	}
	xml_sax_append_string(parser, current);
	return xml_sax_parse(parser, GF_FALSE);
}

GF_EXPORT
GF_Err gf_xml_sax_parse(GF_SAXParser *parser, const void *string)
{
	GF_Err e;
	char *current;
	char *utf_conv = NULL;

	if (parser->unicode_type < 0) return GF_BAD_PARAM;

	if (parser->unicode_type>1) {
		const u16 *sptr = (const u16 *)string;
		u32 len = 2 * gf_utf8_wcslen(sptr);
		utf_conv = (char *)gf_malloc(sizeof(char)*(len+1));
		len = gf_utf8_wcstombs(utf_conv, len, &sptr);
		if (len == GF_UTF8_FAIL) {
			parser->sax_state = SAX_STATE_SYNTAX_ERROR;
			gf_free(utf_conv);
			return GF_CORRUPTED_DATA;
		}
		utf_conv[len] = 0;
		current = utf_conv;
	} else {
		current = (char *)string;
	}

	e = gf_xml_sax_parse_intern(parser, current);
	if (utf_conv) gf_free(utf_conv);
	return e;
}


GF_EXPORT
GF_Err gf_xml_sax_init(GF_SAXParser *parser, unsigned char *BOM)
{
	u32 offset;
	if (!BOM) {
		parser->unicode_type = 0;
		parser->sax_state = SAX_STATE_ELEMENT;
		return GF_OK;
	}

	if (parser->unicode_type >= 0) return gf_xml_sax_parse(parser, BOM);

	if ((BOM[0]==0xFF) && (BOM[1]==0xFE)) {
		if (!BOM[2] && !BOM[3]) return GF_NOT_SUPPORTED;
		parser->unicode_type = 2;
		offset = 2;
	} else if ((BOM[0]==0xFE) && (BOM[1]==0xFF)) {
		if (!BOM[2] && !BOM[3]) return GF_NOT_SUPPORTED;
		parser->unicode_type = 1;
		offset = 2;
	} else if ((BOM[0]==0xEF) && (BOM[1]==0xBB) && (BOM[2]==0xBF)) {
		/*we handle UTF8 as asci*/
		parser->unicode_type = 0;
		offset = 3;
	} else {
		parser->unicode_type = 0;
		offset = 0;
	}

#ifdef GPAC_ENABLE_COVERAGE
	if (gf_sys_is_cov_mode()) {
		format_sax_error(NULL, 0, "");
	}
#endif

	parser->sax_state = SAX_STATE_ELEMENT;
	return gf_xml_sax_parse(parser, BOM + offset);
}

static void xml_sax_reset(GF_SAXParser *parser)
{
	while (1) {
		XML_Entity *ent = (XML_Entity *)gf_list_last(parser->entities);
		if (!ent) break;
		gf_list_rem_last(parser->entities);
		if (ent->name) gf_free(ent->name);
		if (ent->value) gf_free(ent->value);
		gf_free(ent);
	}
	if (parser->buffer) gf_free(parser->buffer);
	parser->buffer = NULL;
	parser->current_pos = 0;
	gf_free(parser->attrs);
	parser->attrs = NULL;
	gf_free(parser->sax_attrs);
	parser->sax_attrs = NULL;
	parser->nb_alloc_attrs = parser->nb_attrs = 0;
}


static GF_Err xml_sax_read_file(GF_SAXParser *parser)
{
	GF_Err e = GF_EOS;
	unsigned char szLine[XML_INPUT_SIZE+2]={0};

#ifdef NO_GZIP
	if (!parser->f_in) return GF_BAD_PARAM;
#else
	if (!parser->gz_in) return GF_BAD_PARAM;
#endif


	while (!parser->suspended) {
#ifdef NO_GZIP
		s32 read = (s32)gf_fread(szLine, XML_INPUT_SIZE, parser->f_in);
#else
		s32 read = gf_gzread(parser->gz_in, szLine, XML_INPUT_SIZE);
#endif
		if ((read<=0) /*&& !parser->node_depth*/) break;
		szLine[read] = 0;
		szLine[read+1] = 0;
		e = gf_xml_sax_parse(parser, szLine);
		if (e) break;
		if (parser->file_pos > parser->file_size) parser->file_size = parser->file_pos + 1;
		if (parser->on_progress) parser->on_progress(parser->sax_cbck, parser->file_pos, parser->file_size);
	}

#ifdef NO_GZIP
	if (gf_feof(parser->f_in)) {
#else
	if (gf_gzeof(parser->gz_in)) {
#endif
		if (!e) e = GF_EOS;
		if (parser->on_progress) parser->on_progress(parser->sax_cbck, parser->file_size, parser->file_size);

#ifdef NO_GZIP
		gf_fclose(parser->f_in);
		parser->f_in = NULL;
#else
		gf_gzclose(parser->gz_in);
		parser->gz_in = 0;
#endif

		parser->elt_start_pos = parser->elt_end_pos = 0;
		parser->elt_name_start = parser->elt_name_end = 0;
		parser->att_name_start = 0;
		parser->current_pos = 0;
		parser->line_size = 0;
		parser->att_sep = 0;
		parser->file_pos = 0;
		parser->file_size = 0;
		parser->line_size = 0;
	}
	return e;
}

GF_EXPORT
GF_Err gf_xml_sax_parse_file(GF_SAXParser *parser, const char *fileName, gf_xml_sax_progress OnProgress)
{
	FILE *test;
	GF_Err e;
	u64 filesize;
#ifndef NO_GZIP
	gzFile gzInput;
#endif
	unsigned char szLine[6];

	parser->on_progress = OnProgress;

	if (!strncmp(fileName, "gmem://", 7)) {
		u32 size;
		u8 *xml_mem_address;
		e = gf_blob_get(fileName, &xml_mem_address, &size, NULL);
		if (e) return e;

		parser->file_size = size;
		//copy possible BOM
		memcpy(szLine, xml_mem_address, 4);
		szLine[4] = szLine[5] = 0;

		parser->file_pos = 0;
		parser->elt_start_pos = 0;
		parser->current_pos = 0;

		e = gf_xml_sax_init(parser, szLine);
        if (!e) {
            e = gf_xml_sax_parse(parser, xml_mem_address+4);
            if (parser->on_progress) parser->on_progress(parser->sax_cbck, parser->file_pos, parser->file_size);
        }
        gf_blob_release(fileName);

		parser->elt_start_pos = parser->elt_end_pos = 0;
		parser->elt_name_start = parser->elt_name_end = 0;
		parser->att_name_start = 0;
		parser->current_pos = 0;
		parser->line_size = 0;
		parser->att_sep = 0;
		parser->file_pos = 0;
		parser->file_size = 0;
		parser->line_size = 0;
		return e;
	}

	/*check file exists and gets its size (zlib doesn't support SEEK_END)*/
	test = gf_fopen(fileName, "rb");
	if (!test) return GF_URL_ERROR;

	filesize = gf_fsize(test);
	gf_fatal_assert(filesize < 0x80000000);
	parser->file_size = (u32) filesize;
	gf_fclose(test);

	parser->file_pos = 0;
	parser->elt_start_pos = 0;
	parser->current_pos = 0;
	//open file and copy possible BOM
#ifdef NO_GZIP
	parser->f_in = gf_fopen(fileName, "rt");
	if (gf_fread(szLine, 4, parser->f_in) != 4) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_CORE, ("[XML] Error loading BOM\n"));
	}
#else
	gzInput = gf_gzopen(fileName, "rb");
	if (!gzInput) return GF_IO_ERR;
	parser->gz_in = gzInput;
	/*init SAX parser (unicode setup)*/
	gf_gzread(gzInput, szLine, 4);
#endif

	szLine[4] = szLine[5] = 0;
	e = gf_xml_sax_init(parser, szLine);
	if (e) return e;

	return xml_sax_read_file(parser);
}

GF_EXPORT
Bool gf_xml_sax_binary_file(GF_SAXParser *parser)
{
	if (!parser) return GF_FALSE;
#ifdef NO_GZIP
	return GF_FALSE;
#else
	if (!parser->gz_in) return GF_FALSE;
	return (((z_stream*)parser->gz_in)->data_type==Z_BINARY) ? GF_TRUE : GF_FALSE;
#endif
}

GF_EXPORT
GF_SAXParser *gf_xml_sax_new(gf_xml_sax_node_start on_node_start,
                             gf_xml_sax_node_end on_node_end,
                             gf_xml_sax_text_content on_text_content,
                             void *cbck)
{
	GF_SAXParser *parser;
	GF_SAFEALLOC(parser, GF_SAXParser);
	if (!parser) return NULL;
	parser->entities = gf_list_new();
	parser->unicode_type = -1;
	parser->sax_node_start = on_node_start;
	parser->sax_node_end = on_node_end;
	parser->sax_text_content = on_text_content;
	parser->sax_cbck = cbck;
	if (!XML_MAX_CONTENT_SIZE) {
		XML_MAX_CONTENT_SIZE = gf_opts_get_int("core", "xml-max-csize");
	}
	return parser;
}

GF_EXPORT
void gf_xml_sax_del(GF_SAXParser *parser)
{
	xml_sax_reset(parser);
	gf_list_del(parser->entities);
#ifdef NO_GZIP
	if (parser->f_in) gf_fclose(parser->f_in);
#else
	if (parser->gz_in) gf_gzclose(parser->gz_in);
#endif
	gf_free(parser);
}

GF_EXPORT
GF_Err gf_xml_sax_suspend(GF_SAXParser *parser, Bool do_suspend)
{
	parser->suspended = do_suspend;
	if (!do_suspend) {
#ifdef NO_GZIP
		if (parser->f_in) return xml_sax_read_file(parser);
#else
		if (parser->gz_in) return xml_sax_read_file(parser);
#endif
		return xml_sax_parse(parser, GF_FALSE);
	}
	return GF_OK;
}


GF_EXPORT
u32 gf_xml_sax_get_line(GF_SAXParser *parser) {
	return parser->line + 1 ;
}

#if 0 //unused
u32 gf_xml_sax_get_file_size(GF_SAXParser *parser)
{
#ifdef NO_GZIP
	return parser->f_in ? parser->file_size : 0;
#else
	return parser->gz_in ? parser->file_size : 0;
#endif
}

u32 gf_xml_sax_get_file_pos(GF_SAXParser *parser)
{
#ifdef NO_GZIP
	return parser->f_in ? parser->file_pos : 0;
#else
	return parser->gz_in ? parser->file_pos : 0;
#endif
}
#endif



GF_EXPORT
char *gf_xml_sax_peek_node(GF_SAXParser *parser, char *att_name, char *att_value, char *substitute, char *get_attr, char *end_pattern, Bool *is_substitute)
{
	u32 state, att_len, alloc_size, _len;
#ifdef NO_GZIP
	u64 pos;
#else
	z_off_t pos;
#endif
	Bool from_buffer;
	Bool dobreak=GF_FALSE;
	char *szLine1, *szLine2, *szLine, *cur_line, *sep, *start, first_c, *result;


#define CPYCAT_ALLOC(__str, __is_copy) _len = (u32) strlen(__str);\
							if ( _len + (__is_copy ? 0 : strlen(szLine))>=alloc_size) {\
								alloc_size = 1 + (u32) strlen(__str);	\
								if (!__is_copy) alloc_size += (u32) strlen(szLine); \
								szLine = gf_realloc(szLine, alloc_size);	\
							}\
							if (__is_copy) { memmove(szLine, __str, sizeof(char)*_len); szLine[_len] = 0; }\
							else strcat(szLine, __str); \

	from_buffer=GF_FALSE;
#ifdef NO_GZIP
	if (!parser->f_in) from_buffer=GF_TRUE;
#else
	if (!parser->gz_in) from_buffer=GF_TRUE;
#endif

	result = NULL;

	szLine1 = gf_malloc(sizeof(char)*(XML_INPUT_SIZE+2));
	if (!szLine1) return NULL;
	szLine2 = gf_malloc(sizeof(char)*(XML_INPUT_SIZE+2));
	if (!szLine2) {
		gf_free(szLine1);
		return NULL;
	}
	szLine1[0] = szLine2[0] = 0;
	pos=0;
	if (!from_buffer) {
#ifdef NO_GZIP
		pos = gf_ftell(parser->f_in);
#else
		pos = (u32) gf_gztell(parser->gz_in);
#endif
	}
	att_len = (u32) strlen(parser->buffer + parser->att_name_start);
	if (att_len<2*XML_INPUT_SIZE) att_len = 2*XML_INPUT_SIZE;
	alloc_size = att_len;
	szLine = (char *) gf_malloc(sizeof(char)*alloc_size);
	if (!szLine) {
		gf_free(szLine1);
		gf_free(szLine2);
		return NULL;
	}
	strcpy(szLine, parser->buffer + parser->att_name_start);
	cur_line = szLine;
	att_len = (u32) strlen(att_value);
	state = 0;
	goto retry;

	while (1) {
		u32 read;
		u8 sep_char;
		if (!from_buffer) {
#ifdef NO_GZIP
			if (gf_feof(parser->f_in)) break;
#else
			if (gf_gzeof(parser->gz_in)) break;
#endif
		}

		if (dobreak) break;

		if (cur_line == szLine2) {
			cur_line = szLine1;
		} else {
			cur_line = szLine2;
		}
		if (from_buffer) {
			dobreak=GF_TRUE;
		} else {
#ifdef NO_GZIP
			read = (u32)gf_fread(cur_line, XML_INPUT_SIZE, parser->f_in);
#else
			read = gf_gzread(parser->gz_in, cur_line, XML_INPUT_SIZE);
#endif
			cur_line[read] = cur_line[read+1] = 0;

			CPYCAT_ALLOC(cur_line, 0);
		}

		if (end_pattern) {
			start  = strstr(szLine, end_pattern);
			if (start) {
				start[0] = 0;
				dobreak = GF_TRUE;
			}
		}

retry:
		if (state == 2) goto fetch_attr;
		sep = strstr(szLine, att_name);
		if (!sep && !state) {
			state = 0;
			start = strrchr(szLine, '<');
			if (start) {
				CPYCAT_ALLOC(start, 1);
			} else {
				CPYCAT_ALLOC(cur_line, 1);
			}
			continue;
		}
		if (!state) {
			state = 1;
			/*load next line*/
			first_c = sep[0];
			sep[0] = 0;
			start = strrchr(szLine, '<');
			if (!start)
				goto exit;
			sep[0] = first_c;
			CPYCAT_ALLOC(start, 1);
			sep = strstr(szLine, att_name);
		}
		sep = sep ? strchr(sep, '=') : NULL;
		if (!sep) {
			state = 0;
			CPYCAT_ALLOC(cur_line, 1);
			continue;
		}
		while (sep[0] && (sep[0] != '\"') && (sep[0] != '\'') ) sep++;
		if (!sep[0]) continue;
		sep_char = sep[0];
		sep++;
		while (sep[0] && strchr(" \n\r\t", sep[0]) ) sep++;
		if (!sep[0]) continue;
		if (!strchr(sep, sep_char))
			continue;

		/*found*/
		if (!strncmp(sep, att_value, att_len)) {
			u32 sub_pos;
			sep = szLine + 1;
			while (strchr(" \t\r\n", sep[0])) sep++;
			sub_pos = 0;
			while (!strchr(" \t\r\n", sep[sub_pos])) sub_pos++;
			first_c = sep[sub_pos];
			sep[sub_pos] = 0;
			state = 2;
			if (!substitute || !get_attr || strcmp(sep, substitute) ) {
				if (is_substitute) *is_substitute = GF_FALSE;
				result = gf_strdup(sep);
				sep[sub_pos] = first_c;
				goto exit;
			}
			sep[sub_pos] = first_c;
fetch_attr:
			sep = strstr(szLine + 1, get_attr);
			if (!sep) {
				CPYCAT_ALLOC(cur_line, 1);
				continue;
			}
			sep += strlen(get_attr);
			while (strchr("= \t\r\n", sep[0])) sep++;
			sep++;
			sub_pos = 0;
			while (!strchr(" \t\r\n/>", sep[sub_pos])) sub_pos++;
			sep[sub_pos-1] = 0;
			result = gf_strdup(sep);
			if (is_substitute) *is_substitute = GF_TRUE;
			goto exit;
		}
		state = 0;
		CPYCAT_ALLOC(sep, 1);
		goto retry;
	}
exit:
	gf_free(szLine);
	gf_free(szLine1);
	gf_free(szLine2);

	if (!from_buffer) {
#ifdef NO_GZIP
		gf_fseek(parser->f_in, pos, SEEK_SET);
#else
		gf_gzrewind(parser->gz_in);
		gf_gzseek(parser->gz_in, pos, SEEK_SET);
#endif
	}
	return result;
}

GF_EXPORT
const char *gf_xml_sax_get_error(GF_SAXParser *parser)
{
	return parser->err_msg;
}


struct _peek_type
{
	GF_SAXParser *parser;
	char *res;
};

static void on_peek_node_start(void *cbk, const char *name, const char *ns, const GF_XMLAttribute *attributes, u32 nb_attributes)
{
	struct _peek_type *pt = (struct _peek_type*)cbk;
	if (pt->res) gf_free(pt->res);
	pt->res = gf_strdup(name);
	pt->parser->suspended = GF_TRUE;
}

GF_EXPORT
char *gf_xml_get_root_type(const char *file, GF_Err *ret)
{
	GF_Err e;
	struct _peek_type pt;
	pt.res = NULL;
	pt.parser = gf_xml_sax_new(on_peek_node_start, NULL, NULL, &pt);
	e = gf_xml_sax_parse_file(pt.parser, file, NULL);
	if (ret) *ret = e;
	gf_xml_sax_del(pt.parser);
	return pt.res;
}


GF_EXPORT
u32 gf_xml_sax_get_node_start_pos(GF_SAXParser *parser)
{
	return parser->elt_start_pos;
}

GF_EXPORT
u32 gf_xml_sax_get_node_end_pos(GF_SAXParser *parser)
{
	return parser->elt_end_pos;
}

struct _tag_dom_parser
{
	GF_SAXParser *parser;
	GF_List *stack;
	//root node being parsed
	GF_XMLNode *root;
	//usually only one :)
	GF_List *root_nodes;
	u32 depth;
	Bool keep_valid;
	void (*OnProgress)(void *cbck, u64 done, u64 tot);
	void *cbk;
};


GF_EXPORT
void gf_xml_dom_node_reset(GF_XMLNode *node, Bool reset_attribs, Bool reset_children)
{
	if (!node) return;
	if (node->attributes && reset_attribs) {
		while (gf_list_count(node->attributes)) {
			GF_XMLAttribute *att = (GF_XMLAttribute *)gf_list_last(node->attributes);
			gf_list_rem_last(node->attributes);
			if (att->name) gf_free(att->name);
			if (att->value) gf_free(att->value);
			gf_free(att);
		}
	}

	if (reset_children && node->content) {
		while (gf_list_count(node->content)) {
			GF_XMLNode *child = (GF_XMLNode *)gf_list_last(node->content);
			gf_list_rem_last(node->content);
			gf_xml_dom_node_del(child);
		}
	}
}

GF_EXPORT
void gf_xml_dom_node_del(GF_XMLNode *node)
{
	if (!node) return;
	gf_xml_dom_node_reset(node, GF_TRUE, GF_TRUE);
	if (node->attributes) gf_list_del(node->attributes);
	if (node->content) gf_list_del(node->content);
	if (node->ns) gf_free(node->ns);
	if (node->name) gf_free(node->name);
	gf_free(node);
}

GF_List * gf_list_new_prealloc(u32 nb_prealloc);

static void on_dom_node_start(void *cbk, const char *name, const char *ns, const GF_XMLAttribute *attributes, u32 nb_attributes)
{
	u32 i;
	GF_DOMParser *par = (GF_DOMParser *) cbk;
	GF_XMLNode *node;

	if (par->root && !gf_list_count(par->stack)) {
		par->parser->suspended = GF_TRUE;
		return;
	}

	GF_SAFEALLOC(node, GF_XMLNode);
	if (!node) {
		par->parser->sax_state = SAX_STATE_ALLOC_ERROR;
		return;
	}
	node->attributes = gf_list_new_prealloc(nb_attributes);
	//don't allocate content yet
	node->name = gf_strdup(name);
	if (ns) node->ns = gf_strdup(ns);
	gf_list_add(par->stack, node);
	if (!par->root) {
		par->root = node;
		gf_list_add(par->root_nodes, node);
	}

	for (i=0; i<nb_attributes; i++) {
		GF_XMLAttribute *att;
		const GF_XMLAttribute *in_att = & attributes[i];
		u32 j;
		Bool dup=GF_FALSE;
		for (j=0;j<i; j++) {
			GF_XMLAttribute *p_att = gf_list_get(node->attributes, j);
			if (!p_att) break;
			if (!strcmp(p_att->name, in_att->name)) {
				dup=GF_TRUE;
				GF_LOG(GF_LOG_DEBUG, GF_LOG_PARSER, ("[SAX] Duplicated attribute \"%s\" on node \"%s\", ignoring\n", in_att->name, name));
				break;
			}
		}
		if (dup) continue;

		GF_SAFEALLOC(att, GF_XMLAttribute);
		if (! att) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_PARSER, ("[SAX] Failed to allocate attribute\n"));
			par->parser->sax_state = SAX_STATE_ALLOC_ERROR;
			return;
		}
		att->name = gf_strdup(in_att->name);
		att->value = gf_strdup(in_att->value);
		gf_list_add(node->attributes, att);
	}
}

static void on_dom_node_end(void *cbk, const char *name, const char *ns)
{
	GF_DOMParser *par = (GF_DOMParser *)cbk;
	GF_XMLNode *last = (GF_XMLNode *)gf_list_last(par->stack);
	gf_list_rem_last(par->stack);

	if (!last || (strlen(last->name)!=strlen(name)) || strcmp(last->name, name) || (!ns && last->ns) || (ns && !last->ns) || (ns && strcmp(last->ns, ns) ) ) {
		s32 idx;
		format_sax_error(par->parser, 0, "Invalid node stack: closing node is %s but %s was expected", name, last ? last->name : "unknown");
		par->parser->suspended = GF_TRUE;
		gf_xml_dom_node_del(last);
		if (last == par->root)
			par->root=NULL;
		idx = gf_list_find(par->root_nodes, last);
		if (idx != -1)
			gf_list_rem(par->root_nodes, idx);
		return;
	}
	if (last != par->root) {
		GF_XMLNode *node = (GF_XMLNode *)gf_list_last(par->stack);
		if (!node->content)
			node->content = gf_list_new();

		gf_list_add(node->content, last);
	}
	last->valid_content = par->keep_valid;
}

static void on_dom_text_content(void *cbk, const char *content, Bool is_cdata)
{
	GF_DOMParser *par = (GF_DOMParser *)cbk;
	GF_XMLNode *node;
	GF_XMLNode *last = (GF_XMLNode *)gf_list_last(par->stack);
	if (!last) return;
	if (!last->content)
		last->content = gf_list_new();

	GF_SAFEALLOC(node, GF_XMLNode);
	if (!node) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_PARSER, ("[SAX] Failed to allocate XML node"));
		par->parser->sax_state = SAX_STATE_ALLOC_ERROR;
		return;
	}
	node->type = is_cdata ? GF_XML_CDATA_TYPE : GF_XML_TEXT_TYPE;
	node->name = gf_strdup(content);
	gf_list_add(last->content, node);
}

GF_EXPORT
GF_DOMParser *gf_xml_dom_new()
{
	GF_DOMParser *dom;
	GF_SAFEALLOC(dom, GF_DOMParser);
	if (!dom) return NULL;

	dom->root_nodes = gf_list_new();
	dom->keep_valid = 0;
	return dom;
}

static void gf_xml_dom_reset(GF_DOMParser *dom, Bool full_reset)
{
	if (full_reset && dom->parser) {
		gf_xml_sax_del(dom->parser);
		dom->parser = NULL;
	}

	if (dom->stack) {
		while (gf_list_count(dom->stack)) {
			GF_XMLNode *n = (GF_XMLNode *)gf_list_last(dom->stack);
			gf_list_rem_last(dom->stack);
			if (dom->root==n) {
				gf_list_del_item(dom->root_nodes, n);
				dom->root = NULL;
			}
			gf_xml_dom_node_del(n);
		}
		gf_list_del(dom->stack);
		dom->stack = NULL;
	}
	if (full_reset && gf_list_count(dom->root_nodes) ) {
		while (gf_list_count(dom->root_nodes)) {
			GF_XMLNode *n = (GF_XMLNode *)gf_list_last(dom->root_nodes);
			gf_list_rem_last(dom->root_nodes);
			gf_xml_dom_node_del(n);
		}
		dom->root = NULL;
	}
}

GF_EXPORT
void gf_xml_dom_del(GF_DOMParser *parser)
{
	if (!parser)
		return;

	gf_xml_dom_reset(parser, GF_TRUE);
	gf_list_del(parser->root_nodes);
	gf_free(parser);
}

GF_EXPORT
GF_XMLNode *gf_xml_dom_detach_root(GF_DOMParser *parser)
{
	GF_XMLNode *root;
	if (!parser)
		return NULL;
	root = parser->root;
	gf_list_del_item(parser->root_nodes, root);
	parser->root = gf_list_get(parser->root_nodes, 0);
	return root;
}

static void dom_on_progress(void *cbck, u64 done, u64 tot)
{
	GF_DOMParser *dom = (GF_DOMParser *)cbck;
	dom->OnProgress(dom->cbk, done, tot);
}

GF_EXPORT
GF_Err gf_xml_dom_parse(GF_DOMParser *dom, const char *file, gf_xml_sax_progress OnProgress, void *cbk)
{
	GF_Err e;
	gf_xml_dom_reset(dom, GF_TRUE);
	dom->stack = gf_list_new();
	dom->parser = gf_xml_sax_new(on_dom_node_start, on_dom_node_end, on_dom_text_content, dom);
	dom->OnProgress = OnProgress;
	dom->cbk = cbk;
	e = gf_xml_sax_parse_file(dom->parser, file, OnProgress ? dom_on_progress : NULL);
	gf_xml_dom_reset(dom, GF_FALSE);
	return e<0 ? e : GF_OK;
}

GF_EXPORT
GF_Err gf_xml_dom_parse_string(GF_DOMParser *dom, char *string)
{
	GF_Err e;
	gf_xml_dom_reset(dom, GF_TRUE);
	dom->stack = gf_list_new();
	dom->parser = gf_xml_sax_new(on_dom_node_start, on_dom_node_end, on_dom_text_content, dom);
	e = gf_xml_sax_init(dom->parser, (unsigned char *) string);
	gf_xml_dom_reset(dom, GF_FALSE);
	return e<0 ? e : GF_OK;
}

GF_EXPORT
GF_Err gf_xml_dom_enable_passthrough(GF_DOMParser *dom)
{
	if (!dom) return GF_BAD_PARAM;
	dom->keep_valid = 1;
	return GF_OK;
}

#if 0 //unused
GF_XMLNode *gf_xml_dom_create_root(GF_DOMParser *parser, const char* name) {
	GF_XMLNode * root;
	if (!parser) return NULL;

	GF_SAFEALLOC(root, GF_XMLNode);
	if (!root) return NULL;
	root->name = gf_strdup(name);

	return root;
}
#endif

GF_EXPORT
GF_XMLNode *gf_xml_dom_get_root(GF_DOMParser *parser)
{
	return parser ? parser->root : NULL;
}
GF_EXPORT
const char *gf_xml_dom_get_error(GF_DOMParser *parser)
{
	return gf_xml_sax_get_error(parser->parser);
}
GF_EXPORT
u32 gf_xml_dom_get_line(GF_DOMParser *parser)
{
	return gf_xml_sax_get_line(parser->parser);
}

GF_EXPORT
u32 gf_xml_dom_get_root_nodes_count(GF_DOMParser *parser)
{
	return parser? gf_list_count(parser->root_nodes) : 0;
}

GF_EXPORT
GF_XMLNode *gf_xml_dom_get_root_idx(GF_DOMParser *parser, u32 idx)
{
	return parser ? (GF_XMLNode*)gf_list_get(parser->root_nodes, idx) : NULL;
}


static void gf_xml_dom_node_serialize(GF_XMLNode *node, Bool content_only, Bool no_escape, char **str, u32 *alloc_size, u32 *size)
{
	u32 i, count, vlen, tot_s;
	char *name;

#define SET_STRING(v)	\
	vlen = (u32) strlen(v);	\
	tot_s = vlen + (*size); \
	if (tot_s >= (*alloc_size)) {	\
		(*alloc_size) = MAX(tot_s, 2 * (*alloc_size) ) + 1;	\
		(*str) = gf_realloc((*str), (*alloc_size));	\
	}	\
	memcpy((*str) + (*size), v, vlen+1);	\
	*size += vlen;	\

	switch (node->type) {
	case GF_XML_CDATA_TYPE:
		SET_STRING("![CDATA[");
		SET_STRING(node->name);
		SET_STRING("]]>");
		return;
	case GF_XML_TEXT_TYPE:
		name = node->name;
		if ((name[0]=='\r') && (name[1]=='\n'))
			name++;

		if (no_escape) {
			SET_STRING(name);
		} else {
			u32 tlen;
			char szChar[2];
			szChar[1] = 0;
			tlen = (u32) strlen(name);
			for (i= 0; i<tlen; i++) {
				switch (name[i]) {
				case '&':
					SET_STRING("&amp;");
					break;
				case '<':
					SET_STRING("&lt;");
					break;
				case '>':
					SET_STRING("&gt;");
					break;
				case '\'':
					SET_STRING("&apos;");
					break;
				case '\"':
					SET_STRING("&quot;");
					break;

				default:
					szChar[0] = name[i];
					SET_STRING(szChar);
					break;
				}
			}
		}
		return;
	}

	if (!content_only) {
		SET_STRING("<");
		if (node->ns) {
			SET_STRING(node->ns);
			SET_STRING(":");
		}
		SET_STRING(node->name);
		count = gf_list_count(node->attributes);
		if (count > 0) {
			SET_STRING(" ");
		}
		for (i=0; i<count; i++) {
			GF_XMLAttribute *att = (GF_XMLAttribute*)gf_list_get(node->attributes, i);
			SET_STRING(att->name);
			SET_STRING("=\"");
			SET_STRING(att->value);
			SET_STRING("\" ");
		}

		if (!gf_list_count(node->content)) {
			SET_STRING("/>");
			return;
		}
		SET_STRING(">");
	}

	count = gf_list_count(node->content);
	for (i=0; i<count; i++) {
		GF_XMLNode *child = (GF_XMLNode*)gf_list_get(node->content, i);
		gf_xml_dom_node_serialize(child, GF_FALSE, node->valid_content, str, alloc_size, size);
	}
	if (!content_only) {
		SET_STRING("</");
		if (node->ns) {
			SET_STRING(node->ns);
			SET_STRING(":");
		}
		SET_STRING(node->name);
		SET_STRING(">");
	}
}

GF_EXPORT
char *gf_xml_dom_serialize(GF_XMLNode *node, Bool content_only, Bool no_escape)
{
	u32 alloc_size = 0;
	u32 size = 0;
	char *str = NULL;
	gf_xml_dom_node_serialize(node, content_only, no_escape, &str, &alloc_size, &size);
	return str;
}

GF_EXPORT
char *gf_xml_dom_serialize_root(GF_XMLNode *node, Bool content_only, Bool no_escape)
{
	u32 alloc_size, size;
	char *str = NULL;
	gf_dynstrcat(&str, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", NULL);
	if (!str) return NULL;

	alloc_size = size = (u32) strlen(str);
	alloc_size = size + 1;
	gf_xml_dom_node_serialize(node, content_only, no_escape, &str, &alloc_size, &size);
	return str;
}

#if 0 //unused
GF_XMLAttribute *gf_xml_dom_set_attribute(GF_XMLNode *node, const char* name, const char* value) {
	GF_XMLAttribute *att;
	if (!name || !value) return NULL;
	if (!node->attributes) {
		node->attributes = gf_list_new();
		if (!node->attributes) return NULL;
	}

	att = gf_xml_dom_create_attribute(name, value);
	if (!att) return NULL;
	gf_list_add(node->attributes, att);
	return att;
}

GF_XMLAttribute *gf_xml_dom_get_attribute(GF_XMLNode *node, const char* name) {
	u32 i = 0;
	GF_XMLAttribute *att;
	if (!node || !name) return NULL;

	while ( (att = (GF_XMLAttribute*)gf_list_enum(node->attributes, &i))) {
		if (!strcmp(att->name, name)) {
			return att;
		}
	}

	return NULL;
}

#endif

GF_EXPORT
GF_XMLAttribute *gf_xml_dom_create_attribute(const char* name, const char* value) {
	GF_XMLAttribute *att;
	GF_SAFEALLOC(att, GF_XMLAttribute);
	if (!att) return NULL;

	att->name = gf_strdup(name);
	att->value = gf_strdup(value);
	return att;
}


GF_EXPORT
GF_Err gf_xml_dom_append_child(GF_XMLNode *node, GF_XMLNode *child) {
	if (!node || !child) return GF_BAD_PARAM;
	if (!node->content) {
		node->content = gf_list_new();
		if (!node->content) return GF_OUT_OF_MEM;
	}
	return gf_list_add(node->content, child);
}

#if 0
/*!
\brief Removes the node to the list of children of this node.

Removes the node to the list of children of this node.
\warning Doesn't free the memory of the removed children.

\param node the GF_XMLNode node
\param child the GF_XMLNode child to remove
\return Error code if any, otherwise GF_OK
 */
GF_EXPORT
GF_Err gf_xml_dom_rem_child(GF_XMLNode *node, GF_XMLNode *child) {
	s32 idx;
	if (!node || !child || !node->content) return GF_BAD_PARAM;
	idx = gf_list_find(node->content, child);
	if (idx == -1) return GF_BAD_PARAM;
	return gf_list_rem(node->content, idx);
}
#endif //unused


GF_XMLNode *gf_xml_dom_node_new(const char* ns, const char* name)
{
	GF_XMLNode* node;
	GF_SAFEALLOC(node, GF_XMLNode);
	if (!node) return NULL;
	if (ns) {
		node->ns = gf_strdup(ns);
		if (!node->ns) {
			gf_free(node);
			return NULL;
		}
	}

	if (name) {
		node->name = gf_strdup(name);
		if (!node->name) {
			gf_free(node->ns);
			gf_free(node);
			return NULL;
		}
		node->type = GF_XML_NODE_TYPE;
	} else {
		node->type = GF_XML_TEXT_TYPE;
	}
	return node;
}

GF_Err gf_xml_dom_node_check_namespace(const GF_XMLNode *n, const char *expected_node_name, const char *expected_ns_prefix) {
	u32 i;
	GF_XMLAttribute *att;

	/*check we are processing the expected node*/
	if (expected_node_name && strcmp(expected_node_name, n->name)) {
		return GF_SG_UNKNOWN_NODE;
	}

	/*check for previously declared prefix (to be manually provided)*/
	if (!n->ns) {
		return GF_OK;
	}
	if (expected_ns_prefix && !strcmp(expected_ns_prefix, n->ns)) {
		return GF_OK;
	}

	/*look for new namespace in attributes*/
	i = 0;
	while ( (att = (GF_XMLAttribute*)gf_list_enum(n->attributes, &i)) ) {
		const char *ns;
		ns = strstr(att->name, ":");
		if (!ns) continue;

		if (!strncmp(att->name, "xmlns", 5)) {
			if (!strcmp(ns+1, n->ns)) {
				return GF_OK;
			}
		} else {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[XML] Unsupported attribute namespace \"%s\": ignoring\n", att->name));
			continue;
		}
	}

	GF_LOG(GF_LOG_WARNING, GF_LOG_CORE, ("[XML] Unresolved namespace \"%s\" for node \"%s\"\n", n->ns, n->name));
	return GF_BAD_PARAM;
}

void gf_xml_dump_string(FILE* file, const char *before, const char *str, const char *after)
{
	size_t i;
	size_t len=str?strlen(str):0;

	if (before) {
		gf_fprintf(file, "%s", before);
	}

	for (i = 0; i < len; i++) {
		switch (str[i]) {
		case '&':
			gf_fprintf(file, "%s", "&amp;");
			break;
		case '<':
			gf_fprintf(file, "%s", "&lt;");
			break;
		case '>':
			gf_fprintf(file, "%s", "&gt;");
			break;
		case '\'':
			gf_fprintf(file, "&apos;");
			break;
		case '\"':
			gf_fprintf(file, "&quot;");
			break;

		default:
			gf_fprintf(file, "%c", str[i]);
			break;
		}
	}

	if (after) {
		gf_fprintf(file, "%s", after);
	}
}


GF_XMLNode *gf_xml_dom_node_clone(GF_XMLNode *node)
{
	GF_XMLNode *clone, *child;
	GF_XMLAttribute *att;
	u32 i;
	GF_SAFEALLOC(clone, GF_XMLNode);
	if (!clone) return NULL;

	clone->type = node->type;
	clone->valid_content = node->valid_content;
	clone->orig_pos = node->orig_pos;
	if (node->name)
		clone->name = gf_strdup(node->name);
	if (node->ns)
		clone->ns = gf_strdup(node->ns);

	clone->attributes = gf_list_new();
	i = 0;
	while ((att = gf_list_enum(node->attributes, &i))) {
		GF_XMLAttribute *att_clone;
		GF_SAFEALLOC(att_clone, GF_XMLAttribute);
		if (!att_clone) {
			gf_xml_dom_node_del(clone);
			return NULL;
		}
		att_clone->name = gf_strdup(att->name);
		att_clone->value = gf_strdup(att->value);
		gf_list_add(clone->attributes, att_clone);
	}
	clone->content = gf_list_new();
	i=0;
	while ((child = gf_list_enum(node->content, &i))) {
		GF_XMLNode *child_clone = gf_xml_dom_node_clone(child);
		if (!child_clone) {
			gf_xml_dom_node_del(clone);
			return NULL;
		}
		gf_list_add(clone->content, child_clone);
	}
	return clone;
}
